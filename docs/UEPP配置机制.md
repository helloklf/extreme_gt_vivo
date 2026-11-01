# vivo UEPP / PEM 配置机制分析

> 分析对象:OriginOS(vivo PD2505,平台 SM8850),`PowerEffectManager.apk` v7.0.0
> 分析方法:adb + root 实测、APK 反编译(androguard)、PEM 自身启动日志
> 日期:2026-10-22

## 一句话结论

`/system/etc/uepp/plugins/` 下的所有 XML 配置**只有一个消费者:`PowerEffectManager.apk`(包名 `com.vivo.pem`)**。配置是三层覆盖结构，最高优先级层位于可写的 `/data/bbkcore/pem/uepp/unified/`，**本地放入的 XML 不验签直接生效**——因此改配置完全不需要动系统分区，也不需要 Magisk 模块。

## 谁是 PEM / UEPP

- **UEPP** 是 vivo 的整机功耗/性能管控框架,插件化架构,全部跑在 `com.vivo.pem` 这一个进程里(system 用户、persistent 常驻)。
- 全盘搜索 `/system`、`/system_ext`、`/product`、`/vendor` 确认:没有任何 native 二进制或其他 APK 引用 `etc/uepp`。`vivoperfservice`、`vendor.vivo.hardware.vperf-service-aidl` 等只是 PEM 的**执行端**(接收指令写节点/调频),自己不读这些 XML。
- 关键代码位置(APK 内):
  - `com.vivo.uepp.config.plugin.PluginConfigManager` — 配置加载总控
  - `com.vivo.uepp.config.plugin.ConfigParser` — XML 解析
  - `com.vivo.uepp.config.plugin.UnifiedConfigManager` + 内部类 `UniCfgStore` — unified 层读写
  - `com.vivo.uepp.plugins.StaticPluginManager` — 插件注册

## 插件目录 ↔ 功能对照

| 目录(插件 ID) | 作用 |
|---|---|
| `com.ueppp.misc.cpu.UtilPlugin` | CPU 工作模式与调频(scaling_max_freq、核在线、优先级) |
| `com.ueppp.misc.cpu.Cgroups` | cgroup 分组/调度配置 |
| `com.ueppp.misc.common.WriteNodePlugin` | 按场景直接写 sysfs/proc 节点 |
| `com.ueppp.misc.sensor.BrightnessPlugin` | 亮度限制(高温降亮度等) |
| `com.ueppp.misc.temperature.Temperature` | 温控策略 |
| `com.ueppp.misc.temperature.ThermalTypeConfig` | 温控档位类型配置 |
| `com.ueppp.camera.camera.CameraPlugin` | 相机场景资源调度 |

设备上还有更多插件(`FrozenPlugin`、`EasySharePlugin`、`PowerHalPlugin`、`VincePlugin`、`spe.ScenePlugin` 等),完整清单见 `/system/etc/uepp/plugins/` 和 APK `assets/plugins/`。

## 三层配置加载机制(核心发现)

PEM 启动时 `PluginConfigManager` 对每个插件按优先级加载三层配置,**高层覆盖低层**:

| 层 | 位置 | 说明 |
|---|---|---|
| builtin(最低) | APK 内 `assets/plugins/<插件ID>/default.xml` | 出厂兜底,随 APK 版本走 |
| device | `/system/etc/uepp/plugins/<插件ID>/<机型>.xml` | 机型/平台定制,即我们复制的这批 |
| unified(最高) | `/data/bbkcore/pem/uepp/unified/<插件ID>/<机型>.xml` | **可写层**,云端下发/本地投放 |

启动日志实证(`PluginConfigManager: load ...`):

```
load builtin: plugins/com.ueppp.misc.app.AppSwitchPlugin/default.xml
load device:  /system/etc/uepp/plugins/com.ueppp.misc.cpu.UtilPlugin/SM8850.xml
load unified: /data/bbkcore/pem/uepp/unified/com.ueppp.misc.cpu.UtilPlugin/SM8850.xml
overlay by unified: com.ueppp.misc.cpu.UtilPlugin     ← 覆盖生效
```

### 文件名选择优先级

同一插件目录下放哪个文件名能被选中,由系统属性决定,优先级从高到低:

```
ro.vivo.oem.model      (本机 PD2505D)
ro.vivo.product.model  (本机 PD2505)
ro.vivo.product.platform(本机 SM8850)
ro.vivo.product.solution(本机 QCOM)
default.xml
```

即 `PD2505D.xml > PD2505.xml > SM8850.xml > QCOM.xml > default.xml`。**投放时用和被覆盖文件相同的名字即可。**

### unified 层的本来用途与签名校验

- unified 层设计上是 vivo 云端热更新配置的落地处:`com.vivo.daemonService` 通过 `content://com.vivo.abe.unifiedconfig.provider/configs` 下发 zip 包(identifier 形如 `uepp_plugin_config_primary/net/freedom/perf/...`),PEM 解包后按 `<插件ID>/<时间戳>.xml` 存入 unified 目录,并监听 `com.vivo.daemonService.unifiedconfig.update_finish_broadcast_pemr` 广播热加载。
- **RSA 签名校验发生在下发端(daemonService 侧),PEM 读本地 unified 目录时不验签**——`UniCfgStore.c()` 只是列出 `<插件ID>/` 下的 `.xml` 文件直接加载。实测手工放入文件后日志出现 `overlay by unified`,配置生效。
- 反编译还发现一个“本地文件直接加载”入口(`UnifiedConfigManager.e(File)`,日志 `load local configuration from ...`),但在当前版本 dex 中**没有调用方**,疑似调试残留,不可用。

## 修改配置的正确姿势

配套脚本:`deploy_uepp_configs.sh`(与本文件同目录),自动完成推送、权限修正、PEM 重启、生效验证。

```bash
./deploy_uepp_configs.sh          # 部署 plugins/ 下全部 XML
./deploy_uepp_configs.sh verify   # 查看当前 unified 层内容与生效情况
./deploy_uepp_configs.sh restore  # 撤销(只删部署过的插件目录),还原系统配置
```

手工等价步骤(以 UtilPlugin 为例):

```bash
adb push SM8850.xml /data/local/tmp/
adb shell 'su -c "
D=/data/bbkcore/pem/uepp/unified/com.ueppp.misc.cpu.UtilPlugin
mkdir -p $D && cp /data/local/tmp/SM8850.xml $D/SM8850.xml
chown -R system:system /data/bbkcore/pem/uepp/unified
chmod 700 /data/bbkcore/pem/uepp/unified $D && chmod 600 $D/SM8850.xml
kill -9 $(pidof com.vivo.pem)"'
```

验证是否生效:

```bash
adb shell 'su -c "grep -E \"load unified|overlay by unified\" \
  $(ls -t /data/system/pem/vlog/1500/log/core_boot.* | head -1)"'
```

## PEM 日志位置(排查问题用)

| 路径 | 内容 |
|---|---|
| `/data/system/pem/vlog/1500/log/core_boot.*` | 启动日志,含全部 `load builtin/device/unified` 记录 |
| `/data/system/pem/vlog/1500/log/policy_trace/*` | 策略引擎逐条匹配轨迹 |
| `/data/system/pem/vlog/1500/log/err/*` | 错误日志(配置解析失败等) |
| `/data/system/pem/vlog/1500/job/*` | 统计任务数据 |

## 注意事项

1. **XML 结构不能动**:`<group __id__="..." __ver__="...">`、`<elem __name__="...">` 是 `ConfigParser` 严格解析的,只能改属性值。写坏了对应插件加载失败(启动日志可见),但不会影响系统启动——删掉 unified 里对应文件重启 PEM 即可恢复。
2. **还原 = 删文件**:unified 层只是覆盖,删掉后 PEM 立刻回退到 device/builtin 层,系统分区全程未被修改。
3. **unified 层会“钉住”配置**:投放后若 vivo 云端后续推送同插件更新,会与本地文件共存竞争(按文件名排序挑选),建议调试完成后及时 restore。
4. **PEM 重启方式**:`kill -9 $(pidof com.vivo.pem)` 即可,它是 persistent 应用会自动拉起并重载配置;改动较多时也可直接重启手机。
5. `/data/bbkcore/pem` 属主 `system:system` 权限 0700,投放文件务必修正属主和权限(600/700),否则 PEM 读不到(日志报 `invalid file, possible permission denied`)。
6. 云端下发的 zip 包本身有 RSA 签名(APK `assets/publicKey.txt`),但那是 daemonService 的事;本地投放不走这条链路。

## 附录:定位过程(可复用的调查方法)

1. `ps -A` / `/proc/*/fd` 找打开 uepp 文件的进程 → 无(配置只在启动时读一次)。
2. `grep -rls uepp` 扫各分区二进制 → 无(native 层不读)。
3. `grep -rls ueppp`(插件 ID 前缀)扫 framework/app → 命中 `PowerEffectManager.apk`。
4. APK 字符串提取确认所有插件类名与目录一一对应,并发现第二个路径 `/data/bbkcore/pem/uepp/unified`。
5. 杀掉 PEM 并读其启动日志 `/data/system/pem/vlog/1500/log/core_boot.*`,看到 `load builtin/device` 两级加载记录。
6. androguard 反编译 `UnifiedConfigManager`/`UniCfgStore`,弄清 unified 目录结构、文件名优先级和“本地不验签”。
7. 投放测试文件 → 日志出现 `overlay by unified` → 实锤。
