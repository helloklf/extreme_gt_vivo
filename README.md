# vivo/iQOO 充电调速工具集（以 V2505A / PD2505 为例）

针对 vivo 新固件（vfcs-cms 充电策略架构）的充电速度控制方案。
充电侧改动全部在内存中，重启即还原；Extreme GT 模块走 Magisk/KernelSU systemless
挂载，不改任何分区。

实测环境：V2505A（iQOO 15），PD2505D_A_16.0.24.7.W10，内核 6.12.23-android16-5，KernelSU root。

> **X300（V2509A，MTK）不一样**：`fixed_temp` 是**电池温度**的替代值，`primary_board_temp`
> 才是主板温度且独立可写；没被识别成 vivo 闪充的第三方头（普通 DCP/PD）走 `chg` 参数集，
> 41°C 以下恒 2700mA，温度伪装没有收益；原装头 / PPS / UFCS 头走 `sp`，用 [tpoff.c](src/tpoff/tpoff.c)
> 只压板温即可（原装头实测 10W → 32W）。详见 [docs/X300充电温控分析.md](docs/X300充电温控分析.md)。

## 能做什么

- **动态限流（fccc）**：按电池温度/电压/电量每秒调节 `fchg_fcc_ma`，温度/电压双上限
  取小、峰值锚定 ratchet，可接管充电控速（模块内默认关闭，`tpoff.conf` 设 `fccc=1`
  与 tpoff 二选一开启）。不像 fbpat 那样绑定具体固件构建，参数随时可调——多数场景
  下它可以取代 fbpat（前提是机型的 `fchg_fcc_ma` 节点能实际控制电荷泵通路）。
- **温度阈值平移（tpoff）**：常驻程序把 CMS 使用的板温/电池温整体偏移 ±N°C。
- **精细限流**：`debug_chg_cur` 节点可随时限制电池电流（mA 级）。
- **亮屏充电提速（fbpat.ko，可选）**：原厂亮屏板温表比灭屏保守得多（40°C 时 4500mA
  vs 8400mA）。补丁把内存中的亮屏表改写成灭屏曲线，亮屏充电 14W → 34W（实测）。
  与固件严格绑定，只有确认瓶颈是亮屏表时才需要。
- **停充/恢复**控制与限流归因观测工具链。

所有手段都保留原厂电池曲线、NTC、过压、超时等保护；fbpat 补丁本身只做"全表内容校验一致才覆写"，校验不过则拒绝写入。

## 快速开始

Magisk/KernelSU 刷入 `ExtremeGT_vivo_<version>.zip`（构建见下文「构建 Extreme GT
模块」）。安装期自动探测机型并生成 `tpoff.conf`：默认启用 tpoff 温度偏移；想让
fccc 接管限流，把模块目录 `tpoff.conf` 里的 `fccc=0` 改为 `1` 后重启（与 tpoff
二选一，详见「fccc 动态限流」）。

### 可选：fbpat 亮屏提速补丁

只有确认瓶颈是亮屏 A 表（用 `tools/chg-trace.sh` 抓掉档点定位）且补丁与当前固件
严格匹配时才需要，重启即还原：

```bash
adb push out/fbpat.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/fbpat.ko'
```

其它固件用 `src/fbpat/make-patch.py` 重新生成（见「适配其它设备」），不要直接套用
现成的 fbpat.ko。

## 构建 Extreme GT 模块

在 Linux 构建环境中安装 `zip` 和 `build.sh` 所需的编译工具后，运行：

```bash
bash build-all.sh
```

脚本编译 `tpoff`、`fanctld` 和 `fccc`，然后将 `ExtremeGT_vivo` 目录的内容打包到仓库根目录。
压缩包名为 `ExtremeGT_vivo_<version>.zip`，版本号取自 `ExtremeGT_vivo/module.prop`。

## fccc 动态限流（默认关闭）

`fccc` 每秒读取电池温度（`cms_class/main_batt_temp`，缺失时回退
`power_supply/battery/temp`，同为 0.1°C）、电压（两串电池组机型
直接读 `cms_class/cell1/cell2_voltage` 取较高单节）、电量和充电电流，
向 `vfcs_class/fchg_fcc_ma`
写入限流值（写 0 释放）：温度越高限得越狠；电压接近满电时以本次充电的峰值电流
为锚点逐步下压，回落到安全区再缓慢恢复；两路上限取小，电量高于阈值直接释放
（两串电池组机型耐受更好，释放阈值从 70% 放宽到 82%）。
若 `cms_class/htccc_enable` 为 0（其它程序已解除原厂 45°C 墙），温度包络
整体自动上移 3°C；该节点只在启动和接上充电器时读取，fccc 自己不写。
具体阈值和曲线系数以 [src/fccc/fccc.c](src/fccc/fccc.c) 文件头的宏定义为准，
本文不复述，避免与代码脱节。

开启方式：模块目录的 `tpoff.conf` 设 `fccc=1`，重启后与 `tpoff` 二选一启动
（默认 `fccc=0`）。也可以 root 手动运行模块目录下的 `fccc`，日志输出到标准错误；
模块自动启动时日志写入模块目录下的 `fccc.log`，持续充电日志会持续增长。

使用边界：

- 该节点能否控制某个机型的电荷泵通路需实机验证；fccc 不改变原厂保护，也
  **不能作为独立安全保护**，阈值来自参考算法而非设备安全规范。
- 与温度伪装工具互斥：`tpoff_mode=board`（如 X300）下 `fixed_temp` 会污染电池
  温度读数，fccc 检测到即罢工；`tpoff_mode=both`（如 iQOO15）则跳过该检查。
- 异常退出可能留下限流值未释放，请检查日志和 `fchg_fcc_ma` 当前值。

## 适配其它设备：make-patch.py（fbpat 补丁）

`fbpat.ko` 二进制只适用于上述固件（vermagic/符号 CRC/结构体偏移/表内容全部绑定具体构建）。
同架构新固件用一键脚本重新生成（从仓库根目录运行）：

```bash
# 在线：设备连着 adb（需 root），全流程自动
py -3 src/fbpat/make-patch.py --serial <adb序列号>

# 离线：帮别人构建，让对方提供文件放进一个目录即可
py -3 src/fbpat/make-patch.py --from-dir <目录>
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

| 路径 | 说明 |
|---|---|
| `src/fbpat/` | 亮屏表补丁功能：`make-patch.py`（一键适配脚本：拉取→生成→加载→验证，从仓库根目录运行）、`gen_module.py`（以原厂 .ko 为模板生成可加载模块）、`fbpat.c`（亮屏表补丁）、`fbres.c`（还原模块，免重启恢复原厂表）、`modload.c`/`modload`（finit_module 加载器，排错用，直接报 errno） |
| `src/tpoff/tpoff.c` | 统一版温度偏移守护程序：伪装模式（只写板温 X300 / 板温+电池温 iQOO15）与 `fixed_temp` 写入单位全部由安装期探测决定（`tpoff.conf` → `service.sh` 传参），运行时不探测；写入限幅 15–55°C 且回读校验 |
| `src/fccc/fccc.c` | 按电池温度/电压/电量附加限流的守护程序，默认关闭 |
| `src/fccc_turbo/fccc_turbo.c` | fccc 同包络算法的功率制变体：DCHG_FCC votable force 后端，启动时关 `htccc_enable` 解除 45°C 墙、温控包络上探 48°C，退出时恢复；独立测试工具，不进模块（原理见 `analysis/fchg-45c-cutoff.md`） |
| `src/fanctld/` | 独立风扇控制器：`fanctld.c`（`pop`（ddr tsens，PoP 封装温度）+ 电池两路曲线取最大值，C 静态实现，含起转脉冲/最小占空比/死区）+ `fan.conf` |
| `src/cmsboost/cmsboost.c` | **未验证**：FCC 投票强制 3.5A + 温度看门狗；只证明能改 CC 设定值，没有实测到提速，PPS/UFCS/原装头下不接管 |
| `build.sh` / `build-all.sh` | aarch64 静态程序编译脚本 / Extreme GT 模块一键构建打包 |
| `tools/watch-charge.py` | 充电状态只读采样 |
| `tools/chg-trace.sh` | 设备端每秒采样 vbat/ibat/SOC/温度/投票，自动标出掉档点，用来定位 A 表哪一档在限流 |
| `tools/collect-root.py` | 设备快照采集 |
| `tools/annotate-disassembly.py` | 反汇编+重定位注释工具 |
| `test/sim_fanctld.c` | fanctld 主机端模拟测试（syscall 层换成模拟文件系统） |
| `ExtremeGT_vivo/` | Magisk 模块模板（含版本日志 `changes.log`），`build-all.sh` 打包为 `ExtremeGT_vivo_<version>.zip` |
| `analysis/` | 驱动字符串/符号表/关键函数摘录/设备树 json |
| `docs/X300充电温控分析.md` | X300 充电温控实现、参数集机制与提速突破口 |
| `docs/X300电压阶梯分析.md` | X300 "电压阶梯"（vbat→ibat 阶梯 / CV 浮充 / CP 退出阈值）的可行性、改写面与收益评估 |
| `docs/风扇控制.md` | fanctld 的两条曲线、配置格式、与 tpoff 的配合 |

已删除的废弃路线：自制内核模块（`gen_module` 嫁接在本机加载即 panic）、
`chgpat`/`chgtbl` 原型、以及一次性探测脚本。原因与已验证的表地址见文档 §7。
