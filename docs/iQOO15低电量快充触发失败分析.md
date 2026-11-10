# iQOO 15(PD2505)低电量(≤5%)插入充电器无法触发快充问题分析

> 实测设备:iQOO 15(V2505A / PD2505,SM8850),user 版 + KernelSU root。
> 现象:电量 5% 以内插充电器,大概率不触发快充(走普通 5V 充电),**插拔一次后恢复正常**。
> 方法:`vfcs-core.ko`(快充协议状态机)、`vfcs-fchg-core.ko`、`vfcs-cms-core.ko`、
> `qti_battery_charger.ko` 静态字符串分析 + votable/debugfs 线上实测。
> 证据文件:`analysis/iqoo15-otg/`。
> 日期:2026-11-10

---

## 0. 一句话结论

快充触发是 `vfcs-core` 的**快充协议状态机在充电器插入事件上跑一次性的准入判定**
(admission gate)。≤5% 深度放电时,多个准入项容易瞬时不过(最可疑:
**`vbat too low %dmV, not allow fchg(thr_mv=%d)`**——5% 时 vbat 已掉到门限附近,
插线瞬间系统负载/开机/省电切换把 vbat 拉低越过门限),状态机直接 **exit 不自动重跑**,
退回普通 5V 充电并一直保持;**插拔 = 新的 attach 事件 = 状态机重跑**,此时电池已缓一口气、
系统也空闲了, handshake 成功,快充出现。"需要插拔"不是玄学,是这套"一次性判定、
无周期重试"设计在低电量工况下的确定性行为。

---

## 1. 快充触发链路

```
充电器插入(Type-C CC 检测 → vbus present)
  → qti_battery_charger: bc1.2 / PD / PPS / QC 初测,usb_online=1
  → vfcs-core 快充协议状态机启动(main_sm, /sys/class/vfcs_class/debug_main_sm 可观测)
    ├─ 准入判定(全部通过才允许 fchg,见 §2)
    ├─ cable_id 检测(SBU ADC, detect cable_id timeout,fail 即退出)
    ├─ 协议握手:vivo 私有闪充 / PD / PPS / QC
    │   (handshake fail / handshake timeout,fail / request power timeout,fail)
    └─ 通过 → 直充(fchg/dchg)上线, votable FCHG_POWER_ENABLE 生效
  → CMS: charge_protocol 已知 → vchg_state 生效 → UI 显示快充
```

协议支持位:实测 `/sys/class/vfcs_class/chg_protocol_support = 0xff`(全协议)。

## 1A. 具体门限值(本机 DT 实测,big-endian)

`/proc/device-tree/soc/vfcs/`(快充执行层):

| 属性 | 值 | 含义 |
|---|---|---|
| `vivo,fchg-vbat-limit-thr-mv` | **[3680, 3350]** | 快充电压窗(迟滞对,见下) |
| `vivo,vbat-too-high-thr-mv` | **4555 mV** | vbat 过高,禁快充 |
| `vivo,chg-ic-vbat-too-high-thr-mv` | 4555 mV | 充电 IC 侧同上 |
| `vivo,ap-vbat-cv-thr-mv` | **[3600, 4555, 4605]** | hchg 准入 3600 / CV 点 4555 / 4605 |
| `vivo,adpt-volt-limit-thr-mv` | **[6600, 6600, 6600, 6700]** | 适配器电压底线(按协议),低于则禁 fchg |
| `vivo,fchg-vbus-max-mv` | 11000 mV | 直充母线电压上限 |
| `vivo,fchg-vfloat-max-mv` | 4510 mV | 充电浮压上限(=cms `bat-vfloat-mv`) |
| `vivo,fchg-curr-max-ma` | 17600 mA | 电池侧电流总上限 |
| `vivo,fchg-ibus-max-ma` | 8800 mA | 输入母线电流上限 |
| `vivo,pps-fchg-curr-max-ma` | 10000 mA | PPS 协议电流上限 |
| `vivo,ufcs-fchg-curr-max-ma` | 8000 mA | UFCS 协议电流上限 |
| `vivo,fchg-ic-switch-ibus-thr-ma` | 1250 mA | 充电 IC 并联切换点 |
| `vivo,exit-fchg-ibat-thr-ma` | 1400 mA | 直充退出电流 |
| `vivo,heating-exit-fchg-ibat-thr-ma` | 2000 mA | 电池加热退出电流 |
| `vivo,fchg-vbat-scale-factor(-max)` | [101,208] / [110,240] | vbat 折算系数 |
| `vivo,temp-exception-thr` | **[700×3, 650×2, 1000×2]**(×0.1℃) | 温度异常门限 **65 / 70 / 100 ℃** |
| `vivo,wls-fchg-curr-max-ma` / `-ibus-max` | 7600 / 3800 mA | 无线直充上限 |
| `vivo,wls-fchg-cable-r-mohm` | 111 mΩ | 无线线阻门限 |

`/proc/device-tree/soc/pmic-glink/cms_v5/`(策略/电池加热层):

| 属性 | 值 | 含义 |
|---|---|---|
| `vivo,tbat-heating-data` | **[-200, 0, 200, 325]**(×0.1℃) | 电池加热触发温度档:**-20.0 / 0 / 20.0 / 32.5 ℃** |
| `vivo,tbat-heating-req-ibat-max-ma` | 12000 mA | 加热允许的最大请求电流 |
| `vivo,tbat-heating-tbat-delta` | 5.0 ℃ | 加热回差 |
| `vivo,tbat-heating-tboard-thermal-limit` | **38.0 ℃** | 板温上限,超则停加热 |
| `vivo,tbat-heating-tz-shell-thermal-limit` | **37.0 ℃** | 壳温上限,超则停加热 |
| `vivo,tbat-heating-soc-low/high/max-thermal-limit` | 60 / 65 / 70 % | 加热的 SOC 窗口门限 |
| `vivo,tbat-heating-soc-foolproof-thermal-limit` | 90 % | 防呆 SOC 门限 |
| `vivo,battery0-temp-low-lim` | 1.5 ℃ | 电芯温度低限(均衡判定) |
| `vivo,battery0-temp-diff/change-lim` | 0.5 ℃ / 0.5 ℃ | 双电芯温差/变化限 |

**与症状的定量关系**:快充准入电压窗实测为 **3350~4555 mV**(3680 为同属性内的
迟滞/恢复值,准入取 3350 还是 3680 需一次失败现场日志确认——日志会原样打印
`vbat too low %dmV, not allow fchg(thr_mv=%d)`)。5% 深度放电带载 vbat ≈ 3.40~3.50 V,
距下限仅 50~150 mV;若准入值是 3680,则 5% 时几乎必然被拒(更符合"很大概率"),
且**必须先有小电流预充把电池抬过 3.68 V——而状态机不会自动重试,只能插拔**。
低温叠加时:电池温度低于 0 ℃(或 20 ℃ 档)进入加热逻辑,加热期间电流被
`heating-exit-fchg-ibat-thr`(2000 mA)钳住,同样表现为"不快充"。

## 2. 准入门清单(vfcs-core 字符串证据,逐条原样)

| 门 | 日志指纹 |
|---|---|
| **系统没忙完** | `sys bootup not complete,not allow fchg(time=%ds)` |
| **vbat 过低** | `vbat too low %dmV, not allow fchg(thr_mv=%d)` |
| **vbat 过高** | `vbat too high %dmV, not allow fchg(thr_mv=%d)` / `ex_chg_ic vbat too high` / `PPS vbat too high` |
| **电池温度过低** | `bat_temp too low(%d), not allow fchg(thr=%d)` ← 电池加热场景 |
| 各温度传感器过高 | `bat_temp / bat_board_temp / bat_btb_temp(_m) / usb_conn_temp / usb_conn1_temp / adapter_temp / adapter_conn_temp too high(%d), not allow fchg(thr=%d)` |
| 适配器异常 | `adapter vbus error(%dmV)` / `adapter ibus error(%dma)` / `adapter_conn_temp too high` |
| D+/D- 异常 | `maybe dp_dm error(%dmV), not allow fchg` |
| 电池内阻 | `battery_resistor_detect, not allow hchg` |
| 电流异常 | `ibat too high %dmA, not allow fchg(thr_ma=%d)` |
| 历史异常 | `fchg exception occured, not allow fchg` |
| AP 状态不可读 | `ap_state_info error.skip main work!` ← 判定依赖的 AP 信息拿不到时**直接跳过本次工作** |
| SOC 门(旁路充) | `soc >= BPC_THR_SOC(%d),exit` |

配套的 votable 体系(`/d/pmic-votable/`,实测导出):

- `FCHG_POWER_ENABLE`(快充总使能,现有 OTG_MODE_EVENT 拉禁)
- `FCHG_LPM_DISABLE` / `COMMON_LPM_DISABLE` / `PI_LPM_DISABLE`(低功耗模式关联门)
- `HCHG_DISABLE`:含 `VBAT_LIMIT_EVENT`、`BAT_HEATING_POWER_DETECT_EVENT`、
  `CAMERA_OPEN_EVENT`、`EXCEPTION_EVENT`
- `DCHG_DISABLE`(直充退出,20+ 个投票者):`VBAT_LIMIT_EVENT`、`VBAT_OVP_EVENT`、
  `BAT_DROP_ERROR_EVENT`、`BATTERY_R_DETECT_EVENT`、`DPB_LOW_TEMP_CURRENT_EVENT`、
  `BAT_HEATING_POWER_DETECT_EVENT`、`CABLE_R_EVENT`、`EU_LAW_FCHG_DISABLE_EVENT`、
  `CALLING_EVENT`、`FFC_EVENT`、`AI_SWITCH_EVENT`、`PMIC_INFO_ERROR_EVENT` 等
- `KEEP_CHG_SOC`(保电 SOC)、`USB_ICL`(输入限流,10+ 投票者)

## 3. 为什么偏偏 ≤5% 容易触发

5% 深度放电时的独有工况,恰好同时压中多个门:

1. **vbat 处于门限边缘**:5% 时 vbat ≈ 3.4~3.5V(本机 46% 时实测 3718mV,斜率外推)。
   插线瞬间基带亮屏、省电切换、应用冻结等大负载把 vbat 再拉低几十 mV →
   `vbat too low, not allow fchg` 概率大幅上升。**这是"大概率而非必然"的原因**——
   取决于插线那一刻的系统负载与电池内阻状态。
2. **系统忙**:低电量常伴随省电模式切换/刚开机(低电自动关机后插线开机),
   `sys bootup not complete, not allow fchg(time=%ds)` 直接拒绝。
3. **低温叠加**:冬季 5% 电量出门插线,电池温度低 → `bat_temp too low` 或
   进入电池加热逻辑(`tbat_heating`、CMS `close_heating_for_chg_suspend`、
   `heating_first_trigger_support`),加热期间不走快充。
4. **AP 状态读取失败**:省电切换瞬间服务被冻结,`ap_state_info error` →
   `skip main work!`,本次甚至连判定都没做完整。

## 4. 为什么"插拔"能修好

- 状态机由 **charger attach 事件**驱动;准入失败 → `[SUB_SM] ... exit=%d` 退出,
  退回普通充电后**没有"条件满足后自动重入"的周期逻辑**(模块字符串中无
  resume-fchg / re-check gate 类日志,只有协议内部的 `retry_count` 和
  `wait for request power ready,retry_count=%d` 这类握手内部重试,
  准入层一旦被拒就是终态);
- 插拔产生新的 attach 事件 → 状态机从头跑 → 此时 vbat 已回升、系统已空闲 → 通过。

旁证:同设计的无线侧 pclamp OVP 检测也有 `already detect done, skip it` 的
"一次判定"注释,是 vivo 这套充电状态机的一贯风格。

## 5. 如何抓到实锤(下次复现时)

电量 ≤5% 插线**立即**执行(失败判定日志只在插入后几秒内打印):

```sh
su -c "dmesg | grep -iE \"not allow fchg|not allow hchg|handshake|request power|cable_id|skip main work|sys bootup\""
```

成功的话会看到具体哪一行(`vbat too low 33xx mV, not allow fchg(thr_mv=34xx)` 之类),
配合:

```sh
cat /sys/class/vfcs_class/debug_main_sm      # 状态机现场
cat /d/pmic-votable/FCHG_POWER_ENABLE/status # 谁禁止了快充
cat /d/pmic-votable/DCHG_DISABLE/status
```

即可把本文的"最可疑"变成"实锤",并拿到门限精确值。

## 5A. 原理:为什么低电压不该强制激活快充(结论存档,不再纠结)

这个门不是"低电量故意不给快充"的营销/策略,而是**电池电化学状态的硬约束**,
只是恰好与低电量高度相关:

1. **直充架构决定了大电流只能喂给"就绪"的电池**。快充(fchg/dchg)是充电器高压
   经电荷泵直挂电池,电流 8~17.6A(fchg-curr-max-ma=17600);普通充电是 5V 小电流
   通道(1~2A)。3.35V 门划的就是"电池是否脱离深放唤醒期、能吃大电流"的分界。
2. **深放电池直接上大电流会析锂**:深度放电后负极嵌锂位点被掏空、极化严重,
   大电流使负极电位跌破 0V(对锂),锂离子以金属锂形式沉积在负极表面——
   **永久性容量损失**,枝晶还可能刺穿隔膜导致内短路。
3. **内阻与系统稳定性**:低 SOC 内阻大,同样电流引起更大压摆,瞬时 cell 电压
   可能跌破保护门限造成充电振荡;插线瞬间充电电流与大负载同时取电,容易把
   VBAT 拉崩重启。
4. **预充是锂电三段式充电的标准第一段**(预充 0→~3.3V 小电流 → 恒流 → 恒压),
   3350mV 即预充段结束点,低于它先小电流唤醒(SEI 稳定、极化消退)是行业规范。
5. 对称的 `vbat too high > 4555mV` 禁快充同理:接近充满的电池在 CV 区也吃不动大电流。

**结论:这个门不该被绕过**(绕过有真实的安全与寿命代价)。合理的优化方向是
省掉"手动插拔"这一步(见 §6 的 watchdog 自动重触发),而不是把门限改低。
"插上先慢几分钟再快充"本身就是电池想要的节奏,要优化的只是"需要人拔插"这个交互。

## 6. 缓解/修复方向(给 ExtremeGT 用)

1. **软件侧自动重试**(推荐,无硬件风险):做一个 watchdog——轮询
   `charge_protocol`(或 `/sys/class/vfcs_class/debug_main_sm`),
   发现"已插入充电器(charger_connected)+ charge_protocol=0(普通充电)"持续 N 秒,
   且 SOC<10%,就**模拟一次插拔事件**重触发状态机。重触发手段待验证:
   优先找软件 re-detect 节点(如往 `vfcs_class` 写 debug 触发);
   备选是旁路 qti 层的 typec re-detect;不建议物理方案。
2. **抬/降 vbat 门限侧**:不可行,也不该做——`thr_mv` 来自 DT/固件,且低于门限
   本就不该直充(电池深度放电直接大电流有析锂等安全风险,原理见 §5A),不建议动。
3. **排除法确认主因**:若复现日志显示是 `bat_temp too low` / 加热逻辑,
   则属于低温策略而非 bug,插拔只是碰巧等到加热完成。
4. 与省电模式的交互:`FCHG_LPM_DISABLE` 一族 votable 值得在复现时顺手看一眼,
   确认低电量自动进省电时是否有投票者禁止快充(若有,可从策略侧豁免)。

## 7. 当前状态备注(分析时设备实况)

设备正处有线反充(OTG source VBUS 5.07V/2.26A),因此 votable 显示
`FCHG_POWER_ENABLE: OTG_MODE_EVENT en=1 v=1`(反充时禁快充,互斥,见有线反充报告);
本次未能抓到 5% 插电的失败现场(dmesg 环形缓冲内无充电器插入记录)。
