# GameWatch.apk 逆向报告（方向 1）

日期：2026-11-06  
目标：vivo V2505A，Android 17，设备 `10AFAB03FM003AB`  
范围：只分析 `GameWatch.apk` 及其自带配置；其他 APK 只作为通信对端名称出现，未越界反编译。全程只读设备，未写任何设备节点、属性或文件。

## 1. 结论摘要

**【静态证据 + 本机服务枚举支持的推断】GameWatch 是 vivo 游戏性能策略的用户态核心之一，而不只是统计守护。** 它同时承担：

1. 进程/游戏场景、温度、FPS 等输入的汇聚与子场景选择；
2. 通过直接文件写、`vivo_daemon.service` shell、`VivoPerfManager` 三条路径执行调度策略；
3. 通过 `gamewatch_server` Binder 向 GameLib、SurfaceFlinger 等暴露命令和回调接口；
4. 通过 `resmon`、`perfsdkmon`、`netopt` 三个 Android local socket 接收游戏/SDK 数据；
5. 通过 GameCube Provider、定向广播、Settings 与 GameCube/GameLib 更新体系协作；
6. 采集帧直方图、FAS/KTOP、温度、触控等数据用于异常与整局统计。

**不能由本报告推出的结论：** 不能仅凭 GameWatch 就断言它独占 `/sys/rsc`，也不能把内核节点字段语义、Binder 对端实现、或本机当前实际写入序列视为已经还原。`vivo_rsc` 内部算法不在本任务范围。

## 2. 样本、工具与可复现方法

### 2.1 样本身份

【观察】

```bash
sha256sum analysis/device_pull/perf/GameWatch.apk
# f19877974481972f07d46e5b65b83731c4997b8bafa4a9766392b72d537362b0

unzip -l analysis/device_pull/perf/GameWatch.apk
# classes.dex 2296568 bytes；只有一个 DEX；APK 共 22 个条目
```

- 包名：`com.vivo.gamewatch`
- `versionName=25.0.0.07`，`versionCode=250007`
- `minSdkVersion=28`，`targetSdkVersion=36`
- 来源：`analysis/device_pull/perf/gamewatch_apktool/apktool.yml:8-16`
- 构建信息：`assets/package_apk.inf:13-18` 给出工程 `VivoCode/android_packages_apps_GameWatch`、分支 `baseline_25.0.0.7_96`、commit `37d6b5f650044e242c3e11994b51d9839729f5fb`，并注明 `vivo.minifyEnabled=true`。

### 2.2 反编译路径与限制

【观察】当前 shell 没有 `jadx`、`jadx.bat`、`apktool` 命令；REA JADX provider 返回 `unsupported_provider`：`win32/arm64` 不支持 Android JADX subprocess ownership。已有工具为 Java 17 和 `C:/Users/hello/apktool/apktool.jar`。

【执行】使用 apktool 2.11.1 离线解码，未安装任何依赖：

```bash
java -jar /c/Users/hello/apktool/apktool.jar d \
  analysis/device_pull/perf/GameWatch.apk \
  -o analysis/device_pull/perf/gamewatch_apktool
# I: Using Apktool 2.11.1 on GameWatch.apk with 8 threads
# I: Baksmaling classes.dex...
# I: Decoding AndroidManifest.xml with resources...
```

因此下文引用的是 **baksmali 结果**，不是 Java 反编译结果。大量类名/方法名被 R8 混淆；`Ls/c;`、`Lh/o;` 等名称必须按 smali 原名引用。`SchedPolicy`、`ResourceController`、`GameStatisticManager` 等仅是代码日志字符串提供的语义别名，属于【推断别名】。

## 3. Manifest 与组件结构

主证据：`analysis/device_pull/perf/gamewatch_apktool/AndroidManifest.xml`。

### 3.1 应用级属性

【观察】

- `android:sharedUserId="android.uid.system"`：与系统 UID 共享（Manifest:2）。
- `android:persistent="true"`、`directBootAware="true"`、`defaultToDeviceProtectedStorage="true"`（Manifest:25）。
- Application：`com.vivo.gamewatch.AppDelegate`（Manifest:25）。
- 没有 Activity，没有 Manifest 静态 BroadcastReceiver。
- 多个组件带 `vivo_process="com.vivo.sps"` metadata；这只是包内 metadata，不能单独证明所有代码只在一个进程运行。

【本机只读观察】命令：

```bash
export MSYS_NO_PATHCONV=1
D:/Android/SDK/platform-tools/adb.exe -s 10AFAB03FM003AB shell \
  "ps -A | grep -E 'com.vivo.sps|gamewatch|gamecube|gamelib|gamekit|gamespace|system_server|surfaceflinger'"
```

关键输出：

```text
system  2040  ... S surfaceflinger
system  3839  ... S system_server
system  5054  ... S com.vivo.sps
system  5655  ... S com.vivo.gamewatch
system  5769  ... S com.vivo.sps:rms
```

这证明本机同时存在 `com.vivo.sps`、`com.vivo.gamewatch` 和 `com.vivo.sps:rms` 进程；不能将 `vivo_process` metadata 简化为“只有一个 com.vivo.sps 进程”。

### 3.2 Service / Provider

| 组件 | exported / 权限 | 观察到的职责 |
|---|---|---|
| `EntryService` | `exported=false`；action `com.sp.sdk.PERSISTENT_SERVICE` | `onBind()` 返回 null，`onCreate/onStartCommand` 只有日志；不是调度 Binder 主入口。证据：`smali/com/vivo/gamewatch/EntryService.smali:17-52` |
| `GameDBJobService` | `exported=false`，`android.permission.BIND_JOB_SERVICE` | JobService 声明，统计数据库相关；非 `/sys/rsc` 主路径 |
| `OneFileService` | `exported=true`，action `com.vivo.easyshare.TRANSFER` | 互传备份恢复；未声明独立 receiver 权限，不能把它当调度入口 |
| `UserManualFpsProvider` | `exported=true`，authority `com.vivo.gamewatch.fpsprovider` | 读需 `READFPSSETTING`，写需 `WRITEFPSSETTING`，均为 `signatureOrSystem` |
| `com.vivo.upgrade.library.provider.FileProvider` | `exported=false`，authority `com.vivo.gamewatch.upgrade` | 升级文件共享，`grantUriPermissions=true` |

Manifest 行号分别为 26-30、37-56。

**【关键结构观察】真正的调度服务没有靠 Manifest 暴露，而是 Application 初始化时直接 `ServiceManager.addService("gamewatch_server", binder)`。** 证据：

- `smali/d/a.smali:442-539`：BaseApplication 初始化，先调用 `core/service/d.j()`，随后初始化场景、FrameAware、图形特性、统计等模块。
- `smali/com/vivo/gamewatch/core/service/d.smali:69-100`：注册 `gamewatch_server`。
- `smali/com/vivo/gamewatch/AppDelegate.smali:17-28`：随后调用 `p1/b.z()` 初始化统计/SurfaceFlinger 监听。

### 3.3 自定义权限和主要 uses-permission

【观察】

- 自定义：`GAMELIB`、`READFPSSETTING`、`WRITEFPSSETTING`、`SWAPMODEL` 均为 `signatureOrSystem`。
- 自定义：`com.vivo.gamecube.fpschange.permission`、`com.vivo.gamecube.configchange.permission` 为 `signature`。
- 使用 GameCube Provider 权限：`com.vivo.gamecube.permission.GAME_PROVIDER`、`GAME_SP_PROVIDER`。
- 使用 `KILL_BACKGROUND_PROCESSES`、`REAL_GET_TASKS`、`REGISTER_WINDOW_MANAGER_LISTENERS`、`SEND_BBKLOG`、统一配置 Provider 等权限。
- 未见 `WRITE_SECURE_SETTINGS` 声明，但应用以 system shared UID 运行并调用系统内部 API；具体 SELinux 权限不由 Manifest 决定。

证据：Manifest:3-24。

## 4. 调度执行架构

### 4.1 三条写路径

| 路径 | 代码证据 | 逻辑 |
|---|---|---|
| 直接写文件 | `smali/l2/b.smali:482-553` | `FileOutputStream(path)` + `PrintWriter.write(value)`，无换行追加；异常仅记录并返回 false |
| daemon shell | `smali/l2/e.smali:66-178,551-590` | 获取 `vivo_daemon.service`，调用 `VivoDmServiceProxy.runShell/runShellWithResult`；`d(p1,p2)` 按平台标志在普通命令和编码命令字符串间选择 |
| VivoPerf 框架 | `smali/p2/h.smali:48-256,607-905` | 反射 `com.vivo.framework.vperf.VivoPerfManager`（或旧 `android.util.BoostFramework`），调用 `perfLockAcquire/Release/perfHint/perfCommon/perfGetFeedback/setPerfPath` |

【本机观察】REA `list_adb_services` Evidence `ev_4b742f9141b1f88f57754cc7b002daa0142b83c9ab3946609f06c9e66fcf28ac` 同时枚举到：

- `vivo_daemon.service` → `IVivoDmService`
- `vendor.vivoperfservice` → `com.vivo.framework.vperf.IVivoPerfManager`
- `gamewatch_server` → `com.vivo.gamewatch.common.IGameWatch`
- `vboost` → `com.vivo.vboost.IVBoost`
- `rms` → `com.vivo.rms.IRM`
- `rms_statistics` → `com.vivo.rms.statistics.IGather`
- `SurfaceFlinger` → `android.ui.ISurfaceComposer`

这些运行时记录只证明服务当前注册，不证明每次静态调用都成功。

### 4.2 通用配置分发

【观察】`Ls/c;` 静态初始化检测以下节点是否存在：`rsc_boost`、`rsc_switch`、`rsc_svp`、`afs/gw_hint`、三个 `fas_gc_*`、`qos/qos_state`，见 `smali/s/c.smali:62-224`。

`Ls/c;->M(fd,type,value)`（`smali/s/c.smali:996-1027`）：

- `type="f"`：直接 `Ll2/b;->j(fd,value)`；
- `type="v"`：`setPerfPath(0,"",fd,value)`；
- 其他/缺省：`echo value > fd` 交给 `vivo_daemon.service`。

`NormalConfigBean` 也走同一分发，见 `smali/com/vivo/gamewatch/gamesdk/config/c.smali:240-340`。

**【推断】这解释了为什么 GameWatch 能写大量未在 smali 中硬编码的 `/sys/rsc` 节点：节点和值主要由 XML 策略驱动。**

## 5. 硬编码 `/sys/rsc` 写入与触发条件

下表只列代码能直接证明的值；配置驱动值另见第 6 节。

### 5.1 RSC boost / SVP / QoS

| 节点 | 写入值和条件 | 证据 |
|---|---|---|
| `/sys/rsc/rsc_boost` | `p(pid)`：先写 `rsc_switch=0x6`，再写 `pid 1`；`D(pid)`：先写 `rsc_switch=0x80000006`，再写 `pid 0`；`A(tid)`：写 `tid 0`；`n(pid,mask)`：写 `pid mask`，大于阈值时按十六进制 mask 与节点 max value 做与运算 | `smali/s/c.smali:1971-2013,2064-2109,3202-3286,3340-3386` |
| `/sys/rsc/rsc_svp` | `o(tid,enter)` 写 `tid 1` 或 `tid 0`；`z()` 读取节点列表，对非 -1 的 threadId 逐项写 `threadId 0` | `smali/s/c.smali:1672-1768,3288-3337,3530-3565` |
| `/sys/rsc/rsc_switch` | 常量 `0x6`、`0x80000006`；`z()` 在 `elapsedRealtime >= 300000 ms` 后也可通过 daemon 写后者 | 同上及 `smali/s/c.smali:3530-3555` |
| `/sys/rsc/qos/qos_state` | `H(id)` 写 `"<id> 1 3"`，节点存在才写 | `smali/s/c.smali:2112-2138` |
| `/sys/rsc/svp/imp_pid` | FrameAware 按配置选两个进程/线程 ID，或 `-1 -1`；另有 miHoYo 冷启动/退出特例 | `smali/p0/a.smali:221-641`；`smali/k/d.smali:500-728,900-1003` |
| `/sys/rsc/svp/cpulimit` | AppBoost 条件分支：开始为两组 `2 0/1 3 1 3 1 16 0`，结束为两组 `2 0/1 3 1 3 1 20 4`；异常线程检查另写 `88000060 <tid> 32 4` | `smali/b0/b.smali:3133-3184`；`smali/o0/b.smali:88-174` |
| `/sys/rsc/svp/svp_limit_process` | `s/a.d()` 连续写 `1 com.tencent.tmgp.sgame com.tencent.tmgp.sgamece` 和 `2 ...`；SM8850 配置另有一组包名，见第 6 节 | `smali/s/c.smali:3031-3042`；`smali/s/a.smali:239-256` |
| `/sys/rsc/svp/gpu_reserve` | 初始化/peak boost release 写 `0`，compact memory 路径写 `1`，均受 `Z0` 特性标志约束 | `smali/b0/b.smali:3749-3950,4911-4973,5828-5893` |

**精确触发链：** `Ls/a;->b()` 接收前台/后台事件，`c(pid,enter)` 从 `j/b` 取主进程、render 等 ID；enter 调 `p()`，exit 调 `D()`。`rsc_svp` 只在 `i2/a.o` 或包名为 `com.tencent.tmgp.sgame(.ce)` 的特定路径追加 `o()`，见 `smali/s/a.smali:61-192,197-256`。不能把所有游戏都概括为相同的 SVP 行为。

### 5.2 FAS / AFS / GC / 调试节点

| 节点 | 写入值和条件 | 证据 |
|---|---|---|
| `/sys/rsc/fas/frame_sched` | 多窗口 `FdCmd.when` 决定写入；`pname=tag` 使用当前 pid，附 `fvalue/svalue`；`777/888` 有多窗口引用计数特殊处理 | `smali/c1/d.smali:401-568,569-824` |
| `/proc/sys/walt/rsc_load_enable` | 同一多窗口策略，配置值 0/1；`rsc_load_cpus` 配置为 255 | 同上；`game_multiwin_config.decoded.xml:11-20,46-59` |
| `/sys/rsc/afs/gw_hint` | GC begin 写 `1`，end 写 `0` | `smali/s/c.smali:3073-3110` |
| `/sys/rsc/fas/fas_gc_threads` | GC begin 写传入线程 ID 字符串，end 写 `-1` | `smali/s/c.smali:3112-3128`；调用 `smali/t/o.smali:811-850` |
| `/sys/rsc/afs/fpsgo_perf_data_attr` | QCOM FAS 数据收集结束/异常兜底写 `1` | `smali/u1/c0.smali:2056-2092` |
| `/sys/rsc/fas/param/debug` | QCOM 下写布尔值 0/1 | `smali/v1/r.smali:1276-1316,867-911` |
| `/sys/rsc/trace_on_status` | 非调试模式、原状态 >0、`debug_trace_switch != 1` 且 PerfDogServer 存活时写 `0` | `smali/l1/a.smali:95-143` |

`fas_gc_uclamp` 和 `fas_gc_freq_limit` 在 `Ls/c;` 中仅做存在性检测；但它们或其他节点仍可能由通用 XML 配置写入，不能把“没有硬编码写”误报为“从不写”。

### 5.3 图形特性节点（Application 初始化）

`smali/d/a.smali:535-539` 调 `Le0/e0;->c()`，异步执行 `Le0/e0;->d()`，见 `smali/e0/e0.smali:83-282`：

- `/sys/rsc/vgarph/MEMC`：`vivo.hardware.game.memc` → 1/0
- `/sys/rsc/vgarph/super_resolution`：`vivo.hardware.game.aisr` → 1/0
- `/sys/rsc/vgarph/fast_animation_engine`：`vivo.software.asyncanimboost` → 1/0
- `/sys/rsc/vgarph/independent_grap`：写 `persist.vivo.displayic.ver` 原字符串
- `/sys/rsc/vgarph/v_sync_opt`：`persist.sys.supportvivosync == 1` → 1/0
- `/sys/rsc/vgarph/ray_tracing`：`/system/etc/rt_vrf_param` 与 `/system/etc/rt_vulkan_cache` 均存在 → 1，否则 0

这些是初始化时特性发布，不应描述成逐帧调度。

### 5.4 `sched_boost`、`mbd` 的辨析

- **【观察】`sched_boost` 在 `b0/b` 中只是 `Lp2/h;->a("sched_boost")` 的 perfLock 名称，不是 `/sys/rsc/sched_boost` 路径。** 证据：`smali/b0/b.smali:1274-1280`；反射实现 `smali/p2/h.smali:48-107`。
- **【有界负搜索】GameWatch smali 未发现 `/sys/rsc/mbd` 或独立 `mbd` 节点字符串。** 只能说明此 APK 没有直接证据，不能证明其他组件或内核没有 mbd。
- **【观察】`vboost` 是 ServiceManager 服务名。** `SystemBridgeManager` 在 SDK>=36 使用 `vboost`，旧版本使用 `rms`，见 `smali/com/vivo/gamewatch/common/SystemBridgeManager.smali:31-97,109-184`。它不是 sysfs 节点。

## 6. 自带策略配置：节点、值与场景选择

### 6.1 配置包与解码证据

APK 含 `assets/vge_configs_6154.zip`，其中 `gw_control_8850.xml` 压缩前 6,228,338 bytes。提取后为自定义编码而非明文 XML。

【观察】`smali/h2/a.smali:139-157` 将 ZIP entry 流交给 `g2/a.b()`；`smali/g2/a.smali:32-193` 对每个 4096-byte 分块执行：逐字节按位取反，再按块内索引 `i % 8` 循环左移。按该算法离线还原后，文件以 `<?xml version="1.0" encoding="utf-8"?>` 开头，且可由 ElementTree 解析。

本次产生的本地证据文件：

- `analysis/device_pull/perf/gamewatch_configs/gw_control_8850.decoded.xml`
- `analysis/device_pull/perf/gamewatch_configs/gw_control_6993.decoded.xml`
- `analysis/device_pull/perf/gamewatch_configs/com_vivo_gw_normal_config.decoded.xml`
- `analysis/device_pull/perf/gamewatch_configs/game_multiwin_config.decoded.xml`

对应原始编码文件也保留在同目录。没有修改 APK。

### 6.2 SM8850 基线统计

【观察，Python ElementTree 对 decoded XML 统计】

```text
model = SM8850(41 scenes), DPD2540(33), PD2546(7), PD2547(7),
        PD2618(15), DPD2653(13), PD2505F_EX(2)
rsc_item_occurrences = 49676
unique_rsc_paths = 111
/sys/rsc/fas/enable: 1139 项（0:216，1:923）
/sys/rsc/fas/param/target_fps: 822 项
  值集合：0,45,48,60,72,83,90,93,120,144,165,185
/sys/rsc/qos/in_gamematch: 113 项（0:1，1:112）
/sys/rsc/svp/disable_input_boost: 924 项（0:27，1:897）
/sys/rsc/rsc_boost、rsc_svp: 此 XML 中 0 项（它们由上述硬编码路径负责）
/sys/rsc/mbd: 0 项
```

注意：这是 **APK 内置基线配置覆盖率**，不是运行时写次数；云端/统一配置可更新 `vge_configs.zip`，本机实际策略可能不同。

### 6.3 配置实例

【观察】`gw_control_8850.decoded.xml:4-32` 的 `common` 默认场景：

- `svp/imp_pid=-1 -1`
- `fas/enable=0`
- `qos/qos_cpu_enable=1`
- `qos/in_gamematch=0`
- `svp/bypass_mode=0`、`game_mode=0`、`game_affinity=00`

`commonGame` 在 `gw_control_8850.decoded.xml:66-122`：

- `useShellTemp=true`，`flag=25`；
- `fas/enable=1`、`disable_input_boost=1`；
- `adaptive_migrate=1`；
- 首个 `temp=40` 的候选从 123 行开始，后续还有 `temp=43` 等候选；
- 另有 `perfConfig="[1082146816,...]"` 数组，供 perfLock 使用，具体字段语义需 vivo_rsc/VivoPerf 对端才能确认。

`com_vivo_gw_normal_config.decoded.xml:59` 的 SM8850 命令为：

```text
echo 2 com.kurogame.mingchao com.tencent.tmgp.dfm com.pkwan.op.vivo \
  com.netease.hyxd com.tencent.weishi com.miHoYo.Yuanshen \
  com.miHoYo.GenshinImpact com.miHoYo.hkrpg com.tencent.tmgp.osgame \
  > /sys/rsc/svp/svp_limit_process
```

多窗口实例 `game_multiwin_config.decoded.xml:11-20`：

- enter：`rsc_load_enable=1`、`rsc_load_cpus=255`；
- `frame_sched` 写 `111 <pid> 600 30 30 30 0 0 0`，再写 `888`；
- exit：`rsc_load_enable=0`，写 `222 <pid> 600`，再写 `777`。

这些常量值证明“写法和时机”，其内核字段含义仍需内核侧证据。

### 6.4 场景选择输入

【观察】`Lh/o;->k0(...)`（`smali/h/o.smali:2611-3244`）按包/调整场景遍历 `Subscene`，比较：

- `sceneName`
- `quality`
- `ultraLoad`
- `api`
- `isLive`
- `temp`（大于 100 时先除以 100）
- `mode` 与公共/复杂场景回退规则

`useShellTemp=true` 时通过 `Li2/a;->k()` 取壳温，见 `smali/h/o.smali:2637-2649`。

`handleGameSceneChange` 的日志明确包含 `reason,pid,packageName,gameScene,refreshRate,quality,ultraLoad,apiType,isLive`，并处理 `MSG_TEMP_CHANGED`、`MSG_GAME_SCENE_CHANGED`，见 `smali/h/o.smali:4800-5068`。选中后调用 `Ls/c;->K()`，后者经 `N()` 遍历 item 并写节点，见 `smali/s/c.smali:1030-1100,2429-2488`。

**【推断】温度既参与调度子场景选择，也用于统计；FrameAware 的 `imp_pid` 则由包名、当前 FPS/quality 与 FrameAware 配置共同决定。** 证据：`smali/p0/a.smali:221-470` 读取 `h/p` 的 FPS/quality，再匹配配置项。

## 7. 采集数据与用途

| 数据 | 观察到的采集路径 | 用途与边界 |
|---|---|---|
| FPS 与 SurfaceFlinger 回调指标 | `u1/n0.K()` 将自身 `gamewatch_server` Binder、包名、pid 传给 SurfaceFlinger，transaction 为 `base+0x4e2a`；stop 为 `base+0x4e2b`。回调注册名 `sf_fps_monitor`，见 `smali/u1/n0.smali:690-789,880-970`、`smali/p1/b.smali:2032-2128` | `p1/b.y()` 接收至少 4 个参数，把三个整数送到 `u1/n0.j()`；第一个由 `u1/c0.p0()` 明确记录为 game FPS。其余整数不在报告中臆测语义 |
| 帧直方图、boost 时间、FPS 时间映射 | SurfaceFlinger 回调可附 byte[]，`p1/b.H()` → `t1/c.e()` Parcel 解析。`BufferInfoItem` 字段包括 histogram、buffer_level_count、boost_timeout_count、boost_time、boost_allowed_time、fps_time_map | `smali/p1/b.smali:200-245`；`smali/t1/c.smali:55-357,411-503`。属于帧/ boost 统计证据，不是直接逐帧写节点证明 |
| FrameAware runtime | `c1/a` 在前台期间每 4 秒读 `sys/rsc/fas/runtime` 第一个整数并累计 | `smali/c1/a.smali:170-198`；`smali/c1/a$a.smali:37-142`。退出时若实际时长与累计差 >20 秒，回调 `h/o.P0` 进入异常统计，见 `c1/a$b.smali:45-100` |
| FAS / KTOP / perfcnt | 读 `/sys/rsc/ktop`、`/sys/rsc/fas/perfcnt`、`/sys/rsc/afs/fpsgo_perf_data_attr`、`/sys/rsc/svp/imp_pid` | `smali/w1/b.smali:657-829`、`w1/f.smali:471-532`、`w1/d.smali:724`、`w1/e.smali:37`。用于 QCOM FAS/异常数据收集 |
| 壳温/环境/CPU/GPU 温度 | `j0/f1` 依次选择 `tshell_game`、`tshell_shell`、`primary_board_temp`，有订阅者时每 3 秒读取并通知；`i2/e` 还扫描 thermal_zone 和 hwmon | `smali/j0/f1.smali:120-180,242-275,348-617`；`smali/i2/e.smali:112-239`。调度选择证据见第 6.4 节；整局统计另存 start/end/max |
| 触控状态 | 整局统计停止时先 `echo 114 > /sys/touchscreen/ts_super_node`，再读十六进制位图，解析 active/gesture/idle/charge/water | `smali/com/vivo/gamewatch/statistics/whole/touch/a.smali:82-208,259-284`。**只证明统计采集，未证明它直接驱动 rsc 写入** |
| 游戏/SDK 场景、线程与网络信息 | local socket `resmon/perfsdkmon/netopt` 接受连接并解析 JSON；binder `turbo/turbo2` 命令也进入 `gamesdk/mtservice/e` | 见第 8 节。`mtservice/e` 对 turbo perfLock 有 1 秒、3 秒、30 秒、60 秒节流路径，见 `smali/com/vivo/gamewatch/gamesdk/mtservice/e.smali:968-1226` |
| 其他整局数据 | `p1/d.d()` 固定创建 battery/frame/network/traffic/temperature/gameCube/latency 等 collector | `smali/p1/d.smali:282-417`。统计完整性依赖 ConfigParser 开关，不代表全部用于实时调度 |

## 8. 跨组件通信

### 8.1 GameWatch 自己暴露的 Binder

【观察】

- 服务名：`gamewatch_server`
- descriptor：`com.vivo.gamewatch.common.IGameWatch`
- transaction：1=`gather`，2=`gathers`，3=`beginGather`，4=`endGather`，5=`execute`
- 参数：`String` + 自定义 Parcelable `ArgPack`

证据：`smali/com/vivo/gamewatch/common/IGameWatch.smali:9-38`、`GameWatchStub.smali:60-179`、`GameWatchProxy.smali:64-284`。

`execute` 由 `core/service/b` 分发，注册命令包括：

- `turbo`、`turbo2`、`vaptTurbo`
- `callback_register`、`sdkinfo_getter_register`、`vgt_server_register`
- `notify_render_thread`、`notify_target_fps`、`notify_api_type`
- `game_space`、`info_provide`、`control_panel`
- `GameLib`
- `net_opt`、`netcallback`
- `notify_to_app_common` 等

证据：`smali/com/vivo/gamewatch/core/service/b.smali:27-308`。

**【推断】GameLib/游戏 SDK 可通过同一 Binder 协议触发调度或注册回调；具体哪个包调用哪个 transaction，需对端 APK 或运行时 Binder trace，本文不冒充分配。** `GameLibExecutor` 和 `GameSpaceExecutor` 的类名只证明命令处理器存在。

### 8.2 与 SurfaceFlinger

【观察】GameWatch 使用原始 `IBinder.transact`、descriptor `android.ui.ISurfaceComposer`：

- 注册/停止 FPS monitor：传 GameWatch Binder、包名、pid（第 7 节）。
- latency 跟踪：`core/service/c.a()` 传 GameWatch Binder + `PersistableBundle`，transaction 为 `h()+0x4e2c`，见 `smali/com/vivo/gamewatch/core/service/c.smali:58-170`。
- `h()` 根据版本返回 0、1000 或 `0x15f90`，说明交易码带版本偏移，不能硬编码成所有版本通用，见同文件 857-889。

这是一条双向通道：GameWatch 请求 SurfaceFlinger 采集，SurfaceFlinger 再用 `gamewatch_server.gather("sf_fps_monitor",...)` 回调。

### 8.3 与 system_server / vivo 系统服务

| 服务 | GameWatch 中的用法 | 证据边界 |
|---|---|---|
| `vboost`（SDK>=36）/ `rms` | `SystemBridgeManager` 原始 transact：PSS、RunningAppProcessInfo、`readPath`、`setBundle`、设置权限；新交易号 100/101/102/103/104 | `SystemBridgeManager.smali:31-97,195-765`。**GameWatch 本身不证明服务实现位于 system_server**；本机只证明服务注册 |
| `rms_statistics` | `IRmsGather` 的 gather/begin/end/gathers | `RmsGatherManager.smali:179-199` |
| `vivo_daemon.service` | `runShell` / `runShellWithResult`，承载 echo/sysfs 命令 | `l2/e.smali` |
| `vendor.vivoperfservice` | 反射 `VivoPerfManager` | `p2/h.smali`；本机接口枚举吻合 |
| `qos` | 本机存在 `com.vivo.qos.IQoS`，但本报告未建立 GameWatch 直接调用它的代码链 | 不把 `/sys/rsc/qos` 写与 IQoS 调用混为一谈 |
| `vivo_nrt_service` | 本机存在 `vivo.transitionservice.ITransitionService` | **GameWatch smali 未找到 `vivonrtservice`、`vivo_nrt_service` 或该 descriptor 的直接引用，直接通信关系未知** |

### 8.4 与 GameCube / GameLib / GameKit / GameSpace

【观察：GameCube】

- Provider URI：`content://com.vivo.gamecube.provider`、`.../complete_privacy_mode`、`content://com.vivo.gamecube.gamespprovider`，见 `smali/v1/f.smali:19-79`。
- 定向广播 `com.vivo.gamewatch.game.quality`：包名、pid、quality，接收方 `com.vivo.gamecube`，权限 `configchange`，见 `smali/h/f.smali:399-431`。
- 定向广播 `com.vivo.gamewatch.game.targetfps`：包名、pid、targetFps，权限 `fpschange`，见同文件 435-483。
- Settings keys 包括 `gamecube_frame_interpolation`、`gamecube_super_resolution` 等；不能把这些 key 当成独立 IPC 证据以外的“直接写内核”。

【观察：GameLib】

- `z1/e.q()` 向 `com.vivo.gamecube` 发送 `com.vivo.gamewatch.game.gamelibInfo`，携带 RequestMode、UpdateMode、NeedUpdate、UpdateContent、ModelUpdateSuccess、DownloadResultCode，接收权限 `com.vivo.gamewatch.permission.GAMELIB`，见 `smali/z1/e.smali:428-473`。
- `gamewatch_server.execute("GameLib",...)` 另有 `GameLibExecutor`，见 `core/service/b.smali:228-234`。
- 本地升级 ZIP 路径包括 `/data/bbkcore/com_vivo_gamelib_GameLib.zip`、`/data/vivo-common/gamelib/...`，见 `z1/c.smali`。

【有界负搜索】未找到 `com.vivo.gamekit` 包名或直接 `vivo_nrt_service` 引用；`GameSpaceExecutor` 只证明 `game_space` Binder 命令，不能据此断言 vivogamespace 的全部通信方式。

### 8.5 Android local socket（不是 Binder）

【观察】`h/o.G0()` 创建三个 `LocalServerSocket`：

| 名称 | 池大小/处理类 | 证据 |
|---|---|---|
| `resmon` | 4，`o/d` + 每连接 `o/a` | `smali/h/o.smali:1681-1701` |
| `perfsdkmon` | 25，`o/d` + 每连接 `o/a` | 同文件 1703-1720 |
| `netopt` | 1，`o/c` | 同文件 1722-1742 |

`o/b.run()` 接受连接，读取 peer pid/uid，并通过 `e2/b` 映射包名；无法映射时停止处理，见 `smali/o/b.smali:66-116`。

`o/d.a()` 为连接构造 `j/b`（pid、uid、包名、socket、回调）并注册到 `h/o`，见 `smali/o/d.smali:27-92`。`o/a` 和 `o/c` 从 socket 读 JSON，并写回 key/value；`netopt` 处理 IP、network、networkSwitch、gameStatus 等字段，见 `smali/o/c.smali:82-500`。

**【推断】这是游戏内 SDK/native 侧向 GameWatch 上报场景、渲染线程和网络数据的重要带外通道；对端库身份仍需 GameLib/native 库证据确认。**

## 9. 本机只读证据汇总

只执行了 `service list`（REA `list_adb_services`）和 `ps`，没有读取或修改 `/sys/rsc`。

- 服务 Evidence：`ev_4b742f9141b1f88f57754cc7b002daa0142b83c9ab3946609f06c9e66fcf28ac`
- 进程 Evidence：`ev_93663c4d2d2782af4648c9e3266a4e059e99be56b42baee54ff859c4a51d65ae`
- 关键服务已注册：`gamewatch_server`、`vboost`、`rms`、`rms_statistics`、`vivo_daemon.service`、`vendor.vivoperfservice`、`qos`、`vivo_nrt_service`。
- 本机进程：`com.vivo.sps`、`com.vivo.gamewatch`、`com.vivo.sps:rms` 同时存在。

## 10. 未解决问题（unknowns）

1. **运行时真实写入序列未知。** 本次遵循只读要求，没有做 fanotify/inotify、strace、Binder trace，也没有改节点做受控实验；静态可达不等于本机每次执行。
2. **当前生效配置版本未知。** APK 基线是 6154；`f2/a` 还能从 `content://com.vivo.abe.unifiedconfig.provider/configs` 获取更新（`smali/f2/a.smali:49-55,126-163`），本机可能已有更新包。
3. **内核字段语义未由 GameWatch 证明。** 例如 `qos_state` 的 `1 3`、`cpulimit` 各列、`rsc_boost` mask 的 bit 定义，需要 vivo_rsc 内核证据。
4. **服务实现归属不完全。** `vboost/rms/qos/vivo_nrt_service` 的服务端代码不在 GameWatch.apk；不能仅凭服务名断言全部位于 system_server。
5. **对端调用映射不完全。** GameLib/GameKit/GameSpace 各自调用哪些 `gamewatch_server` 命令，需对端代码或 Binder trace。
6. **mbd 无直接证据。** GameWatch smali 和 SM8850 基线中均未发现 mbd；应在其他组件/内核方向继续查，不能据此否定 mbd 存在。
7. **混淆导致语义命名受限。** 报告只将日志字符串明确的类赋予“推断别名”，未把混淆方法名臆译为确定功能。
8. **未做 Java 反编译。** 当前 Windows ARM64 上 REA JADX 不可用，使用 apktool/baksmali；关键结论均引用到具体 smali 方法和行号。

## 11. 交付文件索引

- 本报告：`analysis/device_pull/perf/REPORT_GameWatch.md`
- apktool 输出：`analysis/device_pull/perf/gamewatch_apktool/`
- Manifest：`analysis/device_pull/perf/gamewatch_apktool/AndroidManifest.xml`
- 调度核心：`analysis/device_pull/perf/gamewatch_apktool/smali/s/c.smali`
- 场景选择：`analysis/device_pull/perf/gamewatch_apktool/smali/h/o.smali`
- 文件/daemon/perf 写路径：`smali/l2/b.smali`、`smali/l2/e.smali`、`smali/p2/h.smali`
- Binder：`smali/com/vivo/gamewatch/common/`、`smali/com/vivo/gamewatch/core/service/`
- 解码配置：`analysis/device_pull/perf/gamewatch_configs/*.decoded.xml`
