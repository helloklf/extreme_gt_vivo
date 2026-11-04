# vivo/iQOO 充电调速工具集（以 V2505A / PD2505 为例）

针对 vivo 新固件（vfcs-cms 充电策略架构）的运行时充电速度控制方案。
不改任何分区、不装 Magisk 模块，全部改动都在内存中，重启即还原。

实测环境：V2505A（iQOO 15），PD2505D_A_16.0.24.7.W10，内核 6.12.23-android16-5，KernelSU root。

> **X300（V2509A，MTK）不一样**：`fixed_temp` 是**电池温度**的替代值，`primary_board_temp`
> 才是主板温度且独立可写；没被识别成 vivo 闪充的第三方头（普通 DCP/PD）走 `chg` 参数集，
> 41°C 以下恒 2700mA，温度伪装没有收益；原装头 / PPS / UFCS 头走 `sp`，用 [tpoff.c](tpoff.c)
> 只压板温即可（原装头实测 10W → 32W）。详见 [docs/X300充电温控分析.md](docs/X300充电温控分析.md)。

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

# 2. 温度偏移（默认降温 5°C，USB/WLS online 才生效，拔线自动还原；
#    game_bypass_charge_flag >= 1 时暂停伪装并还原）
adb push tpoff /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/tpoff; nohup /data/local/tmp/tpoff >/dev/null 2>&1 &'
adb shell su -c 'killall tpoff'    # 停止；下次启动会先清除覆盖

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

## 构建 Extreme GT 模块

在 Linux 构建环境中安装 `zip` 和 `build.sh` 所需的编译工具后，运行：

```bash
bash build-all.sh
```

脚本编译 `tpoff`、`fanctld` 和 `fccc`，然后将 `ExtremeGT_vivo` 目录的内容打包到仓库根目录。
压缩包名为 `ExtremeGT_vivo_<version>.zip`，版本号取自 `ExtremeGT_vivo/module.prop`。

## 动态限流（默认关闭）

`fccc` 以 1 秒间隔读取 `main_batt_temp`（0.1°C）、
`battery/voltage_now`（µV，要求单节电池电压）和 `battery/capacity`（%），
写入 `vfcs_class/fchg_fcc_ma`（写 0 表示释放，不使用
`cms_class/debug_chg_cur`）。
它不改变原厂保护；该节点能否控制某个机型的电荷泵通路仍需实机验证，不能作为
独立安全保护。

基准为 15600 mA；温度超过 43.1°C 开始限制，先将基准钳到 15000 mA，
随后每上升 0.1°C 乘以 0.94，超过 44.0°C 的部分再每 0.1°C 乘以 0.85。
温度计算最高取 45.0°C，计算结果低于 5000 mA 时按 0（释放）处理；
降至 42.1°C 或以下才退出温度限速状态。

电压策略是单向 ratchet：上限只降不升。电压高于 4.49 V 时每轮压低 50 mA，
高于 4.40 V 时每轮压低 25 mA，压到 4000 mA 下限为止；
4.31–4.40 V 之间保持不变，低于 4.31 V 或停止充电时重置回基准。
端电压随充电电流本身变化，允许上调会形成正反馈振荡，因此只降不升。

最终取温度与电压上限的较小值；电量高于 70% 时一律写 0，停用强制电流。
目标值变化时立即写入，无变化也每 5 秒重写一次（快充未激活时的写入
可能不生效，需要重写）。拔线时清除温度迟滞与电压 ratchet 状态。
电压读数不可读或不在 2–5 V 范围时停止并保留限流，不把电池组电压自动除以二。

构建并安装模块后，在模块目录的 `tpoff.conf` 中设置 `fccc=1`，
重启后与 `tpoff` 二选一启动；默认 `fccc=0`，使用tpoff。
也可以 root 手动运行模块目录下的 `fccc`，日志输出到标准错误；
模块自动启动时日志写入模块目录下的 `fccc.log`。
充电时每次计算（每秒一次）记录 `temp_0.1C`、`voltage_uV`、`capacity_pct`、
温度和电压各自算出的 `temp_mA`、`volt_mA`、各自的限流状态
`temp_limited`、`volt_limited`、写入前的 `current_mA`、决策值
`requested_mA` 和是否需要写入 `write_needed`（含每 5 秒的周期重写）。
日志在尝试写入前输出，`write_needed=1` 不表示写入成功；写入失败会另行报错。
持续充电会不断增加日志文件大小。

- `fixed_temp` 非零、不可读、温度、电压、电量或充电状态异常时停止，**保留最后写入的限流值**，
  不解除其他程序的设置，也不写 `fchg_fcc_ma`。`fixed_temp=0` 并不能证明温度
  一定未被其他机制伪造。
- `tpoff_mode=both` 且温度偏移非零时，模块启动脚本拒绝启动本程序；
  程序本身也在每次采样前后检查 `fixed_temp`。不要与其他温度伪装工具同时使用。
- 检测其他程序改写 `fchg_fcc_ma` 后退出的检查默认关闭（代码中保留，被注释）。
  该节点没有跨程序的原子所有权，其他程序仍可能与本程序竞争。
- 正常停止或拔线时，仅在读回仍是本程序设置的值时写入 0 释放。
  异常退出或被强制终止可能留下限流；请检查日志和当前值。
- 温度、电压阈值来自参考算法和配置要求，不是经验证的设备安全阈值。该程序不能替代硬件或原厂保护。

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
| `tpoff.c` / `tpoff` | 统一版温度偏移守护程序：伪装模式（只写板温 X300 / 板温+电池温 iQOO15）与 `fixed_temp` 写入单位全部由安装期探测决定（`tpoff.conf` → `service.sh` 传参），运行时不探测；写入限幅 15–55°C 且回读校验 |
| `cmsboost.c` / `cmsboost` | **未验证**：FCC 投票强制 3.5A + 温度看门狗；只证明能改 CC 设定值，没有实测到提速，PPS/UFCS/原装头下不接管 |
| `fanctld.c` / `fan.conf` | 独立风扇控制器：`pop`（ddr tsens，PoP 封装温度）+ 电池两路曲线取最大值，C 静态实现，含起转脉冲/最小占空比/死区 |
| `fccc.c` | 按电池温度/电压/电量附加限流的守护程序，默认关闭 |
| `modload.c` / `modload` | finit_module 加载器（排错用，直接报 errno） |
| `watch-charge.py` | 充电状态只读采样 |
| `tools/chg-trace.sh` | 设备端每秒采样 vbat/ibat/SOC/温度/投票，自动标出掉档点，用来定位 A 表哪一档在限流 |
| `collect-root.py` | 设备快照采集 |
| `annotate-disassembly.py` | 反汇编+重定位注释工具 |
| `analysis/` | 驱动字符串/符号表/关键函数摘录/设备树 json |
| `modules/` | 设备原厂模块副本（分析与模板来源） |
| `docs/X300充电温控分析.md` | X300 充电温控实现、参数集机制与提速突破口 |
| `docs/X300电压阶梯分析.md` | X300 "电压阶梯"（vbat→ibat 阶梯 / CV 浮充 / CP 退出阈值）的可行性、改写面与收益评估 |
| `docs/风扇控制.md` | fanctld 的两条曲线、配置格式、与 tpoff 的配合 |

已删除的废弃路线：自制内核模块（`gen_module` 嫁接在本机加载即 panic）、
`chgpat`/`chgtbl` 原型、以及一次性探测脚本。原因与已验证的表地址见文档 §7。
