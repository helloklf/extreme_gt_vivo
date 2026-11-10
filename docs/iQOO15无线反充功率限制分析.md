# iQOO 15(PD2505)无线充电反充功率限制机制分析

> 实测设备:iQOO 15(V2505A / PD2505,SM8850 平台代号 canoe),内核 6.12.58-android16,
> user 版 + KernelSU root(`su -c` 可用)。
> 方法:线上 sysfs / debugfs / dmesg 实测 + 从 `/vendor/lib/modules` 拉回 6 个充电相关
> `.ko` 与 CPS4021 固件做字符串/重定位/rodata 静态分析。
> 证据文件:`analysis/iqoo15-wls-trx/`(vfcs-wls-core.ko、vfcs-wls-dev.ko、
> vfcs-wls-tx-cps8601.ko、vivo-wls-charger-monitor.ko、vfcs-cms-core.ko、
> vfcs-misc-core.ko、wls_cps4021.bin)。
> 日期:2026-11-10

---

## 0. 一句话结论

iQOO 15 的无线反充由 **CPS4021 TRX 芯片(30W 档)** 执行,功率不是单一开关限制,而是
**五层叠加**:① 芯片/协议协商上限(寄存器 `NEGO_DONE_POWER`,vivo 私有协议需
`BC_TX_ENCRYPTION` 认证才放高功率)→ ② 电池侧放电电流硬顶(电荷泵反向 `set_ilim_ma`,
CMS 测试门限 [100,1500] mA)→ ③ 动态降额(谐振频偏降流 / 结温 / 线圈 NTC / pdet_en)
→ ④ 异物与异常保护(FOD、vrect/mldo OVP,直接关断)→ ⑤ CMS 策略门(OTG 互斥、
SOC<10% 禁开、无线充在线禁反充、关机充电模式禁反充)。
**能否开启由第 ⑤ 层决定,能开多大由 ①② 决定,能开多久由 ③④ 决定。**

---

## 1. 硬件与驱动架构

### 1.1 反充执行芯片:CPS4021

| 项目 | 值 |
|---|---|
| 芯片 | ConvenientPower **CPS4021**(无线收发一体 TRX) |
| I2C | `i2c@988000/vfcs_wls_trx@30`(QUP 总线 9c0000) |
| compatible | `vivo,vfcs-wls-trx` |
| DT 属性 | `vivo,fw-bin-name = "wls_cps4021.bin"`、`vivo,wls-trx-dev-type = 3` |
| 驱动 | `vfcs-wls-trx`(由 `vfcs_wls_dev` 模块提供) |
| 固件 | `/vendor/firmware/wls_cps4021.bin`(33 KB,无明文) |
| power_supply | `/sys/class/power_supply/wls_trx/`(实测 online=1, present=3, model=cps4021) |

`vfcs-wls-dev.ko` 内含字符串 **"TRX 30W Programmer V0.0.5"** —— 硬件定档 30W 级 TRX。
同目录固件还有 `wls_sc96230.bin`(南芯 TRX),应为同平台其他 SKU 备选料,本机未使用。

### 1.2 旁证:CPS8601 笔充驱动在本机是空载

`vfcs-wls-tx-cps8601.ko`(源码路径 `vfcs/vfcs-ex/pen-tx/vfcs-wls-cps8601.c`,
CPS8601 触控笔 TX 驱动)虽被加载,但 **`/sys/bus/i2c/drivers/vfcs-wls-tx-cps8601/` 下无任何绑定设备**
——iQOO 15 没有笔充方案,反充只走 CPS4021。该驱动的降额/超时逻辑
(`tx_power_timeout_work`、`bt_discon_tx_timeout`)对本机不适用。

### 1.3 模块分工

| 模块 | 职责 |
|---|---|
| `vfcs-wls-dev.ko` | CPS4021/SC96230 芯片驱动:TX/RX 模式切换、寄存器读写、FOD 参数、ilim、固件升级 |
| `vfcs-wls-core.ko` | 充电 FSM(收/发共用):req_power_max、低频降流、热降额、pdet、反向 TX 状态机(`vfcs_wls_restart_reverse_tx`) |
| `vivo-wls-charger-monitor.ko` | 无线充监控:握手阶段机(BPP 5W / EPP 10/15/50W)、sha256 认证、PCLAMP OVP、反充重启 |
| `vfcs-cms-core.ko` | 充电管理策略:场景/温控、OTG 互斥、低电量门、`wls_tx_switch` 等 class 属性 |
| `vfcs-wls-tx-cps8601.ko` | 笔充 TX(本机空载) |

---

## 2. 开启链路与策略门(实测)

### 2.1 触发路径

```
echo 1 > /sys/class/cms_debug/wls_tx_switch
  → vfcs-cms: wls_tx_switch_store: wls_tx_switch_on=1
  → vfcs-wls: vfcs_wls_set_property: wls_tx_reverse_switch, val:1
  → vfcs-wls: vfcs_wls_reverse_start_work: <门检查>
  → (通过后) enable wls reverse chg → CPS4021 TX 模式 + 电荷泵反向升压
```

### 2.2 门检查清单(dmesg + 模块字符串双重证据)

| 门 | 证据 | 实测状态 |
|---|---|---|
| **OTG 互斥** | `otg is online, not allow wls reverse`;`persist.vivo.wireless_charge_and_otg_repel=1` | **实测被拦**(`otg_source_vbus_state=1`,只读,ksu root 写 0 被 SELinux 拒) |
| 低电量 | `otg_silicon_batt_low_thr = 10`(`/sys/class/cms_class/`,persist 同名属性) | SOC<10% 禁开 |
| 无线充在线 | `wls online,can not reverse` | 收/发互斥 |
| 关机充电模式 | `power_off_charging_mode,return` | 关机充电不可反充 |
| 总开关 | `/d/battery_charger/block_tx`(debugfs,当前 N) | 调试阻断 |

---

## 3. 功率限制的五层机制

### 3.1 芯片/协议上限(硬件天花板 30W)

CPS4021 完成 Q 值/FOD 检测后与对端协商:
- 寄存器:`REG_NEGO_DONE_POWER` / `REG_TX_POTENTIAL_POWER` / `REG_POWER_PROFILE` / `REG_TX_CHARGING`;
- 标准 Qi 设备走 EPP/BPP 档位协商;对端是 vivo 私有设备时走
  `PROPRIETARY18/58 BC_TX_ENCRYPTION1/2` 加密认证 + sha256 校验
  (`TX RX sha256 fail..` / `hash result is not match:failed!`),
  **认证失败不放开私有高功率档**;
- `set cep timeout`、`dploss_cali_cap_num`(功率损耗校准)参与协商稳定。

### 3.2 电池侧放电电流硬顶

- 驱动:`vfcs-wls-dev.ko` 的 `set_ilim_ma limit max` —— 电荷泵反向工作时对
  输入电流设 ilim 钳位,这是反充整机功率的直接瓶颈;
- CMS 侧配套测试门限:`/sys/class/cms_debug/wls_tx_iin_range` = **`[100, 1500]` mA**
  (DT 属性 `vivo,wls-tx-chg-test-param-thr` 未配置,走模块默认值
  `_wls_tx_chg_test_param_thr_default`;写入格式见 `wls_tx_switch_store` 的 `[1,1],buf[0]=1`);
- 估算:电池 3.85V × 1500mA ≈ **5.8W 整机放电口径**(测试判定值,非精确限流点,
  精确值在驱动 ilim 寄存器里)。

### 3.3 动态降额(vfcs-wls-core FSM)

| 触发 | 日志指纹 |
|---|---|
| 谐振频偏降流 | `low_freq: reduce current to %dmA (reduced %dmA, freq=%dkHz)` |
| 芯片结温 | `die_temp=%d detect high, try to reduce value=%d` |
| 线圈 NTC 越限 | `coil NTC(%d) out off range` |
| 温度削减开关 | `temperature too high[%d], enable pdet_en`(`pdet_en/pdet_b_status` 可在 `/sys/class/cms_class/` 观测) |
| 通信质量 | `cep=%d`、`fsk_timeout`、`read reverse iin/vin failed` |

### 3.4 异物/异常保护(关断而非降额)

- FOD:`set fod_th=%d`、`tx_fod_q_enable`、fod 参数组 c0~c6(gain/offset);
  关断原因位图 `disable tx reason:0x%02x`;
- 过压:`vrect_ovp(_to)`、`mldo_ovp`;
- 状态机兜底:`error case : force disable reverse TX work!!`、
  `vfcs_wls_restart_reverse_tx`(`wls_reverse_restart . skip it...`)。

### 3.5 超时(笔充驱动,本机不适用但同源)

`tx_power_timeout_work` / `cps8601_tx_power_timeout_timer` —— CPS8601 笔充的
TX 限时;CPS4021 路径上以 `cep timeout` / `fsk_timeout` 起同类作用。

---

## 4. 静态扫描出的常量(待语义确认)

`vivo-wls-charger-monitor.ko` `.rodata+0x468` 一张 34 项 u32 表:

```
(3000,4000,5000,8000,8000,10000, 3000,4000,5000,6000,10000, 3000,
 1000×8, 3000,3000,3000, 800,900,1400,2000,2000,2000,3000,3000,
 900,1200,2000)
```

紧挨 `TA_REVERSE_0x04` 字符串(反充相关 TA 命令),尾部 `(3000,900,1200,2000)`
应为该命令参数(电压/电流档)。整体疑似 VBUS 电压档 + IBUS 电流档阈值表,
具体语义需反汇编确认。`vfcs-cms-core.ko` `.data+0x3480`(`wls_tx_switch` 属性尾部)
另有 `(-1,-1,-1,-1,-1,-200,0,150)` 常量组,语义未明。

---

## 5. 关键节点速查

| 节点 | 权限 | 作用 |
|---|---|---|
| `/sys/class/cms_debug/wls_tx_switch` | rw | 反充开关(写 1 实测触发完整链路) |
| `/sys/class/cms_debug/wls_tx_iin_range` | r | TX 电流测试门限 [100,1500]mA |
| `/sys/class/cms_class/otg_silicon_batt_low_thr` | r | SOC<10% 禁反充/OTG 阈值 |
| `/sys/class/cms_class/otg_source_vbus_state` | r | OTG 源 VBUS 状态(互斥门输入,只读) |
| `/sys/class/cms_class/wls_otg_status` | r | wls 视角 OTG 状态 |
| `/sys/class/cms_class/wpc_tx` | rw | WPC TX 触发/查询 |
| `/sys/class/cms_class/bat_wpc_tx_chg_state` | r | 反充中:`chg_state_val`,`input_current` |
| `/sys/class/cms_class/bat_wpc_tx_to_tx_state` | r | TX→TX 状态 |
| `/sys/class/cms_class/wls_pdet_en`、`wls_pdet_b_status` | r | 功率削减开关与状态 |
| `/d/battery_charger/block_tx` | rw | 反充总阻断(debugfs) |
| `/sys/class/wls_class/hi_power` | r | 高功率标志(=0) |
| `/sys/class/wls_class/wls_fod_section` | r | FOD 区段(=0) |
| `/sys/class/wls_class/wls_trx_fw_update_{request,result}` | - | TRX 固件升级 |
| `/sys/class/power_supply/wls_trx/` | r | online/present/model_name |

---

## 6. 未验证项与下一步

1. **协商功率实测值未抓到**:本机无对端设备,`NEGO_DONE_POWER` / `TX_POTENTIAL_POWER`
   寄存器读数、对 vivo 私有设备 vs 标准 Qi 设备的档位差异未知。在设置里打开
   "无线反向充电"并放置对端设备后,抓 `dmesg` 中 `req_power_max %d W`、
   `trx_iin/trx_vin`、`nego_done_power=%dmw`、`fod_th` 即可钉死第 ①② 层数字。
2. **驱动步进值未知**:降流步长、温度阈值、ilim 具体值需反汇编
   `vfcs-wls-dev.ko` / `vfcs-wls-core.ko`(当前环境无 AArch64 反汇编器,
   模块已存 `analysis/iqoo15-wls-trx/`,均 stripped)。
3. **OTG 门语义存疑**:`otg_source_vbus_state=1` 时 usb_online=0,该状态由谁置位
   (UCSI 角色?软件 OTG?)未定位,可能与调试线/altmode 有关。
