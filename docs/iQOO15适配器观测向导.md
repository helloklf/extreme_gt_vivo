# iQOO 15(PD2505)适配器观测向导(stm32_debug 实时充电观测窗)

> 实测设备:iQOO 15(V2505A / PD2505,SM8850),user 版 + KernelSU root。
> 内容:充电系统实时观测节点、适配器温度/功率/降额/保护链的读取与诊断方法,
> 数据出处与日志指纹。
> 日期:2026-11-10。
> 相关文档:[iQOO15充电中后程功率闪崩分析](iQOO15充电中后程功率闪崩分析.md)(本向导
> §3 保护链的实测案例)。

---

## 0. 导读:这些数据从哪来

iQOO 15 的快充通信链是:**适配器 ←VFCP/UFCS 协议→ PI 协议芯片 ←I2C→ AP 内核**,
中间由一颗 STM32 MCU 做桥接与信息汇总。内核把 MCU 侧的实时状态以 sysfs 形式
挂在 `/sys/class/stm32_debug/`(debug 接口,root 可读)——这是观察充电系统的
**单点实时观测窗**:温度、电流、握手、直充状态机、线缆识别全都有,且大部分是
驱动层内部状态,不走 power_supply 的抽象,信息量比 `dumpsys battery` 大一个量级。

注意:凡标"适配器上报"的值(温度/功率/错误码)都是**适配器主动通过协议发来**的,
只在快充通信活跃时刷新;普通 5V 充电/空闲时读到的是最后一次上报值(不刷新是
正常现象,不是传感器故障)。

## 1. 节点速查表(`/sys/class/stm32_debug/`,root 可读)

| 节点 | 单位 | 内容 | 备注 |
|---|---|---|---|
| `adapter_temp` | 0.1℃ | **适配器本体温度**(适配器上报) | 闪崩排查第一指标,THR=850(85.0℃) |
| `adapter_conn_temp` | 0.1℃ | 适配器端连接器温度 | 同上来源 |
| `bat_conn_temp` | 0.1℃ | 电池端连接器温度 | |
| `master_bat_conn_temp` | 0.1℃ | 主电芯连接器温度 | |
| `bat_temp` / `bat_temp_mv` | 0.1℃ / mV | 电池温度/电压(MCU 视角) | 与 cms/power_supply 互为印证 |
| `ibus` | mA | 当前母线输入电流 | |
| `req_ibus` | mA | 当前请求的母线电流 | ibus 长期低于 req_ibus → 适配器在给不出 |
| `main_board1_ibus` / `main_board2_ibus` | mA | 双主板电流分布 | |
| `master_ibus` | mA | 主充路径电流 | |
| `handshake_success` | 0/1 | 快充协议握手状态 | 掉 0 → 协议断了 |
| `dchg_state` | 枚举 | 直充状态机状态 | 配合内核 `SUB_SM:VFCP sub_status` |
| `dchg_exit_event` | 事件码 | **直充退出原因码** | 闪崩后直接看这里 |
| `cable_mohm` | mΩ | 线缆电阻实测 | 本机实测 101~125mΩ |
| `cable_matched` | 0/1 | 线缆识别(是否原装/合格) | |
| `chg_enable` / `master_chg_enable` | 0/1 | 充电使能 | |
| `chg_switch` / `master_chg_online` | 枚举 | 充电路径开关状态 | |
| `sbu_adc` | ADC | SBU 线 ADC 值(cable_id 检测用) | |
| `in_version` / `out_version` | 版本号 | MCU 内外固件版本 | |
| `mcu_manufacturer` / `mcu_fw_inside_burn_result` | — | MCU 厂商/烧录结果 | 出厂相关 |

配套 cms_class 适配器节点:`adapter_power` / `adapter_power_max`(适配器功率档位,
单位 W,本机原装 90/90)、`supported_adapter`、`adapter_error_code2_bigdata_info`。

## 2. 一键监视命令

```sh
# 紧凑版(适配器温度 + 母线电流 + 直充状态)
su -c "while true; do
  T=\$(cat /sys/class/stm32_debug/adapter_temp)
  printf '%s adpt=%d.%dC ibus=%d req=%d dchg=%s exit_ev=%s hs=%s\n' \
    \$(date +%H:%M:%S) \$((T/10)) \$((T%10)) \
    \$(cat /sys/class/stm32_debug/ibus) \
    \$(cat /sys/class/stm32_debug/req_ibus) \
    \$(cat /sys/class/stm32_debug/dchg_state) \
    \$(cat /sys/class/stm32_debug/dchg_exit_event) \
    \$(cat /sys/class/stm32_debug/handshake_success)
  sleep 1
done"
```

长时间记录(抓闪崩现场推荐,断线不丢):

```sh
su -c "nohup dmesg -w > /data/local/tmp/chg_test.log 2>/dev/null &"
```

## 3. 温度→降额→保护链(实测数字,来自闪崩事件日志)

```
适配器温度(0.1℃)        系统行为
  400~790   爬升期      满功率运行(本机 90W 档,入机 ~77W:9926mV×7802mA)
  800       t=10461     ADAPTER_POWER_DERATE_EVENT 生效,ibat 投票压到 6560mA
  850 贴顶  t=10583     顶在 ADPT_TEMP_TOO_HIGH_THR=850;实测贴顶 ~56s 后
  ── 热保护关机 ──     PI request mode(0x02→0x00),ap_exception_reason=0x200000,
                        SUB_SM:VFCP sub_status=16 卸载直充
                        vbus 8.4V→4.97V,ibat→0.2A(用户看到"功率变 0")
  +9.5s     自动恢复    状态机 sub 2→3→6→7→8→9→10→11 重跑握手,功率拉起
  820~830   余热期      重启后余温仍高,降额 6560mA 下继续升温 → 可再次触保
```

实测结论:这颗 90W 适配器在该环境温度下**满功率约能撑 10 分钟**,之后进入
"降额→热保关机→自动恢复→再升温"循环,属正常保护,手机端无需干预。

## 4. 闪崩/不快充诊断流程

1. **先看 `dchg_exit_event`**(stm32_debug)——直充退出原因码,一锤定音入口。
2. **内核日志切窗口**(闪崩前后 ±10s):
   `grep -E "ap_exception_reason|sub_status=|ADAPTER_POWER_DERATE|request mode"`
   - `ap_exception_reason=0x200000`(bit21)= 源异常保护退出;
   - 恢复时同一行会变回 `0x0`,且 SUB_SM 重跑 2→11。
3. **判热因**:`adapter_temp` 是否曾贴 850;贴顶 ~1 分钟内必关机。
   缓解=改善适配器散热,不是修手机。
4. **判市电假摔**(重要,避免误判):日志里 `adpt_input_volt_v last=70`
   之类的"市电跌穿"**大多是采样伪影**——判据:该样本错误码同步为 0x00,
   且当时 vbus/ibat 无任何扰动(真跌落适配器输出必崩)。两个条件都中才是真的。
5. **不快充但功率不为 0**(低电量场景):看 `handshake_success` 与
   `/sys/class/vfcs_class/debug_main_sm`;若握手始终 0,参考
   [低电量快充触发失败分析](iQOO15低电量快充触发失败分析.md)。

## 5. 数据出处(与 sysfs 对应的内核日志指纹)

| 观测项 | 内核日志指纹 | 周期 |
|---|---|---|
| 适配器温度 | `vfcs_check_temp_exception: [TEMP] ... adapter_temp=850` | ~6.6s |
| 适配器错误码 | `vfcs_get_adapter_error_code2: adapter_error_code2:[0]=0x91` | ~6.6s |
| 市电输入电压 | `adpt_input_volt_v: first=218,last=215,min=70,max=218` | ~6.6s |
| 功率降额 | `vfcs-voter: ADAPTER_POWER_DERATE_EVENT:enable=1,value=6560` | 事件触发 |
| 协议状态机 | `vfcs_monitor_fchg_protocol: [SUB_SM:VFCP] main_status=1,sub_status=11,...` | ~1.3s |
| 保护退出 | `ap_exception_reason=0x200000` + `request mode(0x02->0x00)` | 事件触发 |
| 适配器功率档 | `vfcs-cms: ... chg_adapter_power=2000`(log 内字段,单位 10W 档) | ~29s |

原始证据:`analysis/iqoo15-lowsoc-chg.log`(9.7MB,含完整闪崩事件)、
`analysis/iqoo15-lowsoc-stat.log`(1s 采样)。
