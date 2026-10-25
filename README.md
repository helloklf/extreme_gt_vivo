# vivo/iQOO 充电调速工具集（以 V2505A / PD2505 为例）

针对 vivo 新固件（vfcs-cms 充电策略架构）的运行时充电速度控制方案。
不改任何分区、不装 Magisk 模块，全部改动都在内存中，重启即还原。

实测环境：V2505A（iQOO 15），PD2505D_A_16.0.24.7.W10，内核 6.12.23-android16-5，KernelSU root。

## 能做什么

- **亮屏充电提速**：原厂亮屏板温表比灭屏保守得多（40°C 时 4500mA vs 8400mA）。
  补丁把内存中的亮屏表改写成灭屏曲线，亮屏充电 14W → 34W（实测）。
- **精细限流**：`debug_chg_cur` 节点可随时限制电池电流（mA 级）。
- **温度阈值平移**：`tpoff` 常驻程序把 CMS 使用的板温/电池温整体偏移 ±N°C。
- **停充/恢复**控制与限流归因观测工具链。

所有手段都保留原厂电池曲线、NTC、过压、超时等保护；补丁本身只做"全表内容校验一致才覆写"，校验不过则拒绝写入。

## 快速开始

```bash
# 1. 亮屏提速补丁（重启后需重新加载）
adb push fbpat.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/fbpat.ko'

# 还原（免重启）
adb push fbres.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/fbres.ko'

# 2. 温度偏移（默认降温 5°C，插电才生效，拔线/被 kill 自动还原）
adb push tpoff /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/tpoff; nohup /data/local/tmp/tpoff >/dev/null 2>&1 &'
adb shell su -c 'killall tpoff'    # 停止并还原

# 3. 精细限流（mA，立即生效；写 17600 解除）
adb shell su -c 'echo 3000 > /sys/class/cms_class/debug_chg_cur'
```

观测当前限流来自哪里：

```bash
adb shell su -c 'grep effective $(grep -m1 debugfs /proc/mounts | cut -d" " -f2)/pmic-votable/USER_IBAT/status'
```

## 适配其它设备：make-patch.py

`fbpat.ko` 二进制只适用于上述固件（vermagic/符号 CRC/结构体偏移/表内容全部绑定具体构建）。
同架构新固件用一键脚本重新生成：

```bash
# 在线：设备连着 adb（需 root），全流程自动
py -3 make-patch.py --serial <adb序列号>

# 离线：帮别人构建，让对方提供文件放进一个目录即可
py -3 make-patch.py --from-dir <目录>
```

离线目录内容（root 后从设备拉取，或从全量固件包的 vendor_dlkm.img/dtbo 解开）：

- `vfcs-cms-core.ko`——模板与偏移来源（必须）
- `config.gz`（即 `/proc/config.gz`）——CFI/签名策略判断（缺省按最坏情况处理）
- `cmsdt.tar`——`/sys/firmware/devicetree/base` 下 cms_v* 节点的 tar 包（推荐），
  或单独放四个表属性文件：`vivo,sp-primary-fbon-tc-data`、`vivo,sp-primary-fboff-tc-data`、
  `vivo,eq-primary-fbon-tc-data`、`vivo,eq-primary-fboff-tc-data`（原始二进制或同名逗号分隔 .txt）

依赖：Python3 + pyelftools、NDK（clang、llvm-objdump，用 `ANDROID_NDK_HOME` 环境变量或默认
`D:/Android/SDK/ndk` 路径探测）、设备已 root 且内核允许加载无签名模块
（`CONFIG_MODULE_SIG_FORCE` 未开）。

**不适用会自动拒绝，不会写任何内存。** 已知不适用：MTK 平台老架构（cms_v2，无亮灭屏分表）。

## 原理要点

- vivo 新固件的充电策略在 `vfcs-cms-core.ko`（vfcs-cms 驱动），可调属性在
  `/sys/class/cms_class/`（不是 power_supply 下）。
- 限流通过 votable 投票实现：debugfs `pmic-votable/USER_IBAT/status` 的 effective
  行直接显示当前生效的限流者和数值；`debug_chg_cur` 就是其中一类 Min 投票。
- 亮屏/灭屏用不同的主板温度-电流表（DT 里的 `*-primary-fbon/fboff-tc-data`），
  按 `fb_state` 选择。表在驱动 probe 时解析进堆内存，fbpat.ko 就是在内存中把
  fbon 表内容覆写为 fboff 表内容。
- `fbpat.ko` 由 `gen_module.py` 以设备原厂模块为模板生成（继承 struct module 布局、
  vermagic、MODVERSIONS CRC），CFI 哈希取自原厂模块函数前缀——因此无需内核源码树。
- 均衡充电开关（`persist.vivo.enable_equalizing_charge` → `switch_state`）决定用
  eq 还是 sp 表；eq 亮灭屏同样保守。`switch_state=0`（运行时写）可回 sp 表。

## 风险与边界

- **温度伪装（tpoff）会削弱板温/电池温保护**，实质是把温控曲线阈值平移；
  手机明显发热时不要用。fixed_temp 的假温度会进入 Android 过热关机判定
  （>55°C 有实测关机记录），程序已内置 55°C 硬上限。
- fbpat.ko 改动的是亮屏板温曲线选择，高温降档逻辑仍然生效，但亮屏高功率充电
  温升更快，注意散热条件。
- 所有改动均为内存态，重启还原；`fbpat.ko` 需与固件严格匹配，不匹配时校验失败、
  拒绝写入。
- 本仓库的表数据、偏移、哈希均来自 V2505A 当前固件实测，**不是**通用结论；
  其它机型请用 make-patch.py 重新生成，不要直接套用 fbpat.ko。

## 目录

| 文件 | 说明 |
|---|---|
| `make-patch.py` | 一键适配脚本（拉取→生成→加载→验证） |
| `gen_module.py` | 以原厂 .ko 为模板生成可加载模块 |
| `fbpat.c` / `fbpat.ko` | 亮屏表补丁（V2505A 当前固件用） |
| `fbres.c` / `fbres.ko` | 还原模块（免重启恢复原厂表） |
| `tpoff.c` / `tpoff` | 2.2KB 纯 syscall 温度偏移守护程序 |
| `modload.c` / `modload` | finit_module 加载器（排错用，直接报 errno） |
| `watch-charge.py` | 充电状态只读采样 |
| `collect-root.py` | 设备快照采集 |
| `annotate-disassembly.py` | 反汇编+重定位注释工具 |
| `analysis/` | 驱动字符串/符号表/关键函数摘录/设备树 json |
| `modules/` | 设备原厂模块副本（分析与模板来源） |
