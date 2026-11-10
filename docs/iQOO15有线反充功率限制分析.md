# iQOO 15(PD2505)有线反充(USB OTG 对外供电)功率限制机制分析

> 实测设备:iQOO 15(V2505A / PD2505,SM8850 平台代号 canoe),user 版 + KernelSU root。
> 场景:**Type-C 对另一台设备有线反充中**(port0 power_role=source, data_role=host,
> partner 支持 PD)。实测 VBUS 5.07V × 2.26A ≈ **11.5W 输出**,电池侧放电 2.88A@3.75V ≈ 10.8W。
> 方法:sysfs / typec / debugfs / dmesg 线上实测 + `qti_battery_charger.ko`、
> `vivo_usb_intf.ko`、`vfcs-cms-core.ko` 静态字符串分析。
> 证据文件:`analysis/iqoo15-otg/`(qti_battery_charger.ko、vivo_usb_intf.ko)、
> `analysis/iqoo15-wls-trx/vfcs-cms-core.ko`。
> 日期:2026-11-10

---

## 0. 一句话结论

有线反充的电流上限**不在 AP 内核里**,而是 **PMIC(ADSP 侧充电固件)的 OTG boost 限流**;
AP 内核(vivo 改的 `qti_battery_charger` + `vfcs-cms-core`)负责**开关与策略门**:
① 使能链 `cms otg_enable → POWER_SUPPLY_PROP_USB_OTG → pmic-glink → ADSP → PMIC5 boost 输出 5V`;
② **低电量 10% 强制断 VBUS**(`otgBatLowThr`,CMS 周期同步给 ADSP);
③ **互斥门**(插着充电器/无线充在线/无线反充时不允许开 OTG);
④ 另一 SKU 用外置 boost 芯片供电的,有 **OCP 1.5~1.6A 硬保护**,过流直接关断。
实测整机给到对端 ~2.26A,说明本机 PMIC boost 限流 ≥2.4A 档,对端(支持 PD)吃到接近
Type-C 3A 广告电流的上限。

---

## 1. 使能链路与供电路径(实测)

### 1.1 当前状态快照

```
/sys/class/typec/port0/power_role   = [source] sink     ← 对外供电
/sys/class/typec/port0/data_role    = host [device]     ← host 模式
/sys/class/typec/port0-partner/     存在 pd1、usb_power_delivery ← 对端是 PD 设备
/sys/class/power_supply/usb/voltage_now = 5068000 (µV)   ← VBUS 5.07V
/sys/class/power_supply/usb/current_now = 2256000 (µA)   ← 输出 2.256A
healthd: st=3 c=-2880 ... otg=1                          ← 电池放电 2.88A
/sys/class/cms_class/otg_source_vbus_state = 1
/sys/class/power_supply/usb/usb_otg = 1
```

### 1.2 使能链(vivo 定制层)

```
设置里打开 OTG/反充
  → vfcs-cms: otg_enable / otg_power_enable(日志指纹 ## set otg power flag =%d ##)
  → vfcs-cms: T_OTG_CONTROL_EVENT → vfcs_cms_otg_control_work
  → qti_battery_charger: set POWER_SUPPLY_PROP_USB_OTG=%d
  → pmic-glink(battery-charger)→ ADSP 充电固件
  → PMIC5 OTG boost 输出 5V VBUS(本机走 PMIC 内部 boost)
dwc3-msm / vivo_usb_intf("vusb dbg")负责 Type-C CC 硬件检测与数据角色切换:
  dwc3_vusb_otg_mode_show, typec_hw_det:1, typec_cc_open:0
```

关键 dmesg 指纹:`otg preapre, waitting mcu init done...`(部分 SKU 需等 MCU 准备好)、
`otg preapre done!! enable otg now!`、`Couldn't enable OTG rc=%d`。

### 1.3 VBUS 电源:两条硬件路线

| 路线 | 判据 | 本机 |
|---|---|---|
| **PMIC 内部 boost**(默认) | `set POWER_SUPPLY_PROP_USB_OTG` 经 glink 到 ADSP | ✅ 使用中(`extern_boost_regulator_is_enable: [otg_boost_en, otg_ctrl_en]=[0, 0], otg_online=1`,regulator.93 state=disabled) |
| **外置 boost 芯片** | DT `vivo,otg-use-usbin-vout-enable` / `vivo,otg-waitting-mcu-prepare`(本机均为 0),`otg_boost_en_gpio`/`otg_ctrl_en_gpio`/`otg_ocp_int_gpio` | 未使用 |

外置 boost 路线有独立保护:`otg_ocp detect, disable OTG directly, maybe 1.5~1.6A OCP occur!!`
——OCP 中断触发直接关 OTG,即该方案的硬电流墙在 **1.5~1.6A**。

---

## 2. 功率限制的五层机制

### 2.1 硬件电流墙(boost ILIM / OCP)

- **本机(PMIC boost)**:限流寄存器(BOOST_ILIM 类)在 ADSP 充电固件侧配置,AP 不可见;
  实测 2.256A@5.07V 说明 ILIM ≥ 2.4A 档(或为对端按 3A 广告吃满,见 2.2)。AP 侧
  没有任何向 ADSP 下发 OTG 电流限值的通道(misc 参数里只有 `otgBatLowThr`,无电流项)。
- **外置 boost SKU**:OCP 1.5~1.6A 硬关断(见 §1.3)。

### 2.2 Type-C 电流广告 / PD source cap(对端能拿多少)

- 手机作为 source 通过 CC 上拉(Rp)广告电流能力(default / 1.5A / 3A),对端据此取电;
- 对端支持 PD(`port0-partner/pd1` 存在),协商 source capabilities;本机以 5V 档对外;
- 实测 2.26A ≈ 3A 广告档下的实际取电——**对端 PD 策略与线材也是功率分配的一部分**。

### 2.3 低电量强制断 VBUS(策略层,AP↔ADSP)

- CMS 周期同步:`vfcs_cms_update_misc_info_to_adsp: apUIsoc=49, otgBatLowThr=10, ...`
  → ADSP 侧 `vchg_set_misc_param` 生效;
- 阈值节点 `/sys/class/cms_class/otg_silicon_batt_low_thr`(可写),默认 **10**,
  对应 `persist.vivo.otg_silicon_batt_low_thr=10`;
- 保护动作指纹:`disable otg vbus,avoid ocp uvlo`(低电量/欠压风险时 CMS 主动断 VBUS)。

### 2.4 互斥门(能不能开)

| 门 | 日志/字符串证据 |
|---|---|
| 插着充电器 | `usb_present, skip otg enable...`、`wire and otg conflict` |
| 无线充在线 | `wls and wls reverse conflict`;`persist.vivo.wireless_charge_and_otg_repel=1`(无线充与 OTG 互斥)|
| 无线反充在线 | 同上,实测无线反充被 `otg is online` 拦截(见无线反充报告 §2.2)|
| 关机充电模式 | qti_battery_charger `is_atboot, otg_enable=%d` |

反向证据:OTG 开着时(`/sys/class/cms_class/otg_source_vbus_state=1`)无线反充
`reverse_start_work` 必然报 `otg is online, not allow wls reverse`。

### 2.5 温度与其他保护

- CMS 把 `bat_temp` / `bat_board_temp` 同步给 ADSP(见 misc 参数),ADSP 侧充电算法
  决定是否降额/断 VBUS——**具体降额曲线在闭源固件里,AP 不可见**;
- 连接器温度保护:`vfcs_cms_usb_conn_vbusoff_work`(trigger=700, release=620,×0.1℃)
  主要用于充电场景 VBUS 关断,OTG 场景同链路;
- 打开 OTG 会推迟modem状态统计工作 5 分钟:`OTG setting open, md_status work postpone 5min`;
- `hBoost is clamped at %u mV`:振动升压与 OTG 共用升压时的钳位保护。

---

## 3. 观测/调节节点速查

| 节点 | 权限 | 说明 |
|---|---|---|
| `/sys/class/typec/port0/power_role` | rw | source/sink 角色 |
| `/sys/class/typec/port0/data_role` | r | host/device |
| `/sys/class/typec/port0-partner/` | r | 对端 PD 信息 |
| `/sys/class/power_supply/usb/voltage_now` | r | VBUS 电压(µV) |
| `/sys/class/power_supply/usb/current_now` | r | VBUS 输出电流(µA) |
| `/sys/class/power_supply/usb/usb_otg` | r | OTG 状态(=1) |
| `/sys/class/cms_class/otg_source_vbus_state` | r | vivo 视角 OTG 对外供电状态 |
| `/sys/class/cms_class/otg_silicon_batt_low_thr` | **rw** | 低电量断 OTG 阈值(默认 10) |
| `/sys/class/cms_class/wls_otg_status` | r | wls 视角 OTG(互斥门输入) |
| `/sys/class/regulator/regulator.93`(extern_boost_otg_vbus) | r | 外置 boost(本机 disabled) |
| `persist.vivo.phone.otg_param` | 3 | vivo OTG 参数(语义未确认) |
| `persist.vivo.wireless_charge_and_otg_repel` | 1 | 无线充与 OTG 互斥开关 |

---

## 4. 与无线反充的互斥关系(补充)

有线 OTG(反充)与无线反充共用 **PMIC VBUS** 资源:
- `wls_tx_pmic_vbus_is_enabled`、`POWER_SUPPLY_PROP_WLS_PMIC_VBUS_EN` 说明无线反充
  也是借 PMIC VBUS 路径对外输出;
- 因此二者天然互斥,策略层双向拦截(任一开,另一个的 start_work 必被拒)。

---

## 5. 未确认项与下一步

1. **PMIC boost ILIM 具体档位与配置位置**:在 ADSP 充电固件(可能由 UEFI/固件常量
   初始化),AP 无接口可读;要确认"2.26A 是负载还是顶到限流",需加大负载(换更高功耗
   对端)观察 `usb.current_now` 是否平台期。
2. **PD source capabilities 内容**:`port0-partner/pd1` 下可解析对端请求与本机广播的
   5V 档电流(需抓 PD 协议或读 UCSI 寄存器)。
3. **高温降额曲线**:ADSP 侧闭源,只能从 dmesg(`vchgadsp` 标签)侧面观察。
4. `persist.vivo.phone.otg_param=3` 与 `vivo,otg-power-method`(本机 DT 未配置)
   的语义需反汇编 `vfcs-cms-core.ko` 确认。
