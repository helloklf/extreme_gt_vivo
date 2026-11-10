# iQOO 15(PD2505)充电中后程"功率闪崩"事件分析(切断→自动恢复)

> 实测设备:iQOO 15(V2505A / PD2505,SM8850),user 版 + KernelSU root。
> 现象:低电量(3%)起充后约 13 分钟,充电功率四次瞬间归零(vbus 8.4V→5V、
> 电流 15A→0.2A),**约 9.5 秒后自动重新拉起**,全程无需插拔。
> 日期:2026-11-10。
> 原始数据:`analysis/iqoo15-lowsoc-chg.log`(9.7MB 内核日志流)、
> `analysis/iqoo15-lowsoc-stat.log`(1s 周期采样),采集方法见 §1。

---

## 0. 一句话结论

这不是手机故障,也不是伪装温度(tpoff)干扰,而是**适配器过热触发的保护性
重启**:充电约 10 分钟内适配器温度从 40℃ 单调爬升,先到 80℃ 时主动发起功率降额
(`ADAPTER_POWER_DERATE` 压到 6560mA),85℃ 贴顶约 1 分钟后热保护关机(两次,
间隔约 1 分钟)——PI 协议芯片退出 VFCP 快充模式,状态机卸载直充(vbus 落回 5V、
电流归零),**~9.5 秒后自动重新握手、功率恢复**。
"闪崩"是设计好的自我保护;根因是**适配器散热**,与市电电压无关——日志里
`adpt_input_volt last=70` 的"电压跌落"经核实是采样伪影(该样本错误码同步为
0x00 无效值,且对应时刻 vbus/ibat 无任何扰动,真实市电全程 215~218V 稳定)。

## 1. 数据采集方法(可复现)

整个事件是用**手机端后台常驻采集 + 事后回捞**抓到的,不依赖 PC 实时连接,
无线 adb / 断线都不影响:

```sh
# ① 内核日志实时流(dmesg -w 常驻,缓冲整个充电过程)
su -c "nohup dmesg -w > /data/local/tmp/chg_test.log 2>/dev/null &"

# ② 1 秒周期状态采样(电量/充放电状态/VBUS 电压电流/快充状态机)
su -c "nohup sh -c 'while true; do
    echo ---\$(date +%H:%M:%S)--- >> /data/local/tmp/chg_stat.log
    cat /sys/class/power_supply/battery/status \
        /sys/class/power_supply/battery/capacity \
        /sys/class/power_supply/usb/online \
        /sys/class/power_supply/usb/voltage_now \
        /sys/class/power_supply/usb/current_now \
        /sys/class/vfcs_class/debug_main_sm \
        >> /data/local/tmp/chg_stat.log 2>/dev/null
    sleep 1
  done' >/dev/null 2>&1 &"
```

之后正常充电即可。事后:

```sh
adb pull /data/local/tmp/chg_test.log   # 全量内核日志(含 vfcs/voter/协议状态机)
adb pull /data/local/tmp/chg_stat.log   # 1s 粒度的功率轨迹
```

分析手段:按 `[秒.微秒]` 时间戳切窗口,围绕"功率异常点"反向找决策日志。
本次事件的关键检索词:`ap_exception_reason`、`sub_status=16`、
`request mode`、`ADAPTER_POWER_DERATE`、`adapter_error_code2`、
`adpt_input_volt`、`SUB_SM:VFCP`。失败/异常判定日志只在事件瞬间打印,
所以**常驻采集比事后 dmesg 抓现成关键得多**——事后抓只能看到环形缓冲里
幸存的部分。

## 2. 事件时间线(两次完全同构,t≈17:18:22 与 17:19:21)

第一次(内核时间 t=10639~10650,第二次在 10698~10708,全部可从日志复核):

| 时间 | 日志指纹 | 含义 |
|---|---|---|
| t=10457 | `ADAPTER_POWER_DERATE_EVENT:enable=1,value=6560` | 掉功率前约 3 分钟,适配器已被限功率到 6560mA(此前满档 17600) |
| t=10639.11 | `vfcs_pi_gen2_request_mode: request mode(0x02->0x00)` | PI 协议芯片收到指令:退出 VFCP 快充模式(0x02→空闲 0x00) |
| t=10640.17 | `ap_exception_reason=0x200000`;`SUB_SM:VFCP sub_status=16`;`vfcs_sc8550v_hw_init` | **源异常保护触发**:快充状态机走退出态,两颗 SC8550 电荷泵重新初始化 |
| t=10640~10644 | vbus 8420→**4970mV**,ibat -15A→**+0.2A** | 直充卸载,退回 5V 普通充电,电流归零——用户感知的"功率变 0" |
| t=10643.9 | `SUB_SM:VFCP sub 2`(fchg_chg_status=0) | 自动开始**重新检测**,无需插拔 |
| t=10644.8→10649.7 | sub 3→6→7→8→9→10→11,`handshake_success=1` | 完整重跑协议握手(约 6 秒) |
| t=10650 起 | vbus 回升 8.4V+,ibat 逐步回 -15A | 快充恢复,之后持续正常 |

全程断电 **≈9.5 秒**,两次事件间隔约 58 秒(市电/适配器再次异常)。

## 3. 触发链与证据(定案:过热,非市电)

**温度轨迹**(`vfcs_check_temp_exception` 周期采样,真传感器非哨兵值):

```
t= 9938  40℃起测   → 63℃(10153) → 72℃(10245,平台) → 80℃(10461)
t=10583  85.0℃ 贴顶(ADPT_TEMP_TOO_HIGH_THR=850),随后 ~56s 卡在 ceiling
t=10639  第一次热保护关机(adapter 统计窗口重置:first 218→224,error code 0x9a)
t=10644  重启后 83℃ → 降额 6560mA 重新升温 → 83℃ 再次触保(内部热点早于上报温度)
```

**降额是适配器的自救**:t=10468(80℃)`ADAPTER_POWER_DERATE_EVENT` 压到 6560mA,
撑了约 3 分钟没压住,85℃ 贴顶 1 分钟后关机。

**"市电跌落"是伪影,已排除**:`adpt_input_volt_v last=70` 全 session 出现十几次
(t=9980、10128、10222、10503、10616、10686…),但每次伴随两个破绽——
① 同一样本错误码为 0x00(无效值);② 该时刻 vbus/ibat 曲线无任何扰动(真跌到
70V 适配器输出必然崩溃)。真实市电全程 first/last ≈ 218/215V。

**保护决策**:PI 芯片 `request mode(0x02->0x00)` + AP 侧
`ap_exception_reason=0x200000`(bit21,源异常位)→ 状态机 sub_status=16
卸载。退出瞬间 `vfcs_get_mode_error_code: error_code=0x0000` 说明
**PI 芯片自身没有硬件错误**,退出是适配器主动断供后的策略性卸载。

**自愈路径**:卸载后状态机自动回到检测态(sub=2),重跑 2→3→6→7→8→9→10→11
完整握手,期间 `handshake_success=0→1`、`retry_count` 有 1~2 次内部重试,
与插入充电器时的正常激活路径完全一致(见低电量快充报告 §1)。

## 4. 排除项(避免以后再纠结)

- **与 tpoff/温度伪装无关**:事件发生前后 `bat_temp=300(伪装值)/380(真实)`,
  板温 300/371、连接器 439/250、适配器 850,除适配器贴顶外全部远离温度门;
  全日志唯一的 `not allow` 是电池加热门(`close_heating_for_chg_suspend`),
  与本次事件不相干。
- **与 fchg 电压准入门无关**:充电中后程 vbat 已回升至 4.0V+。
- **不是充电器拔出**:全程 `adsp_usb_online=1`,CC 连接未断,只是协议模式退出。
- **不是异常耗电/系统问题**:`exception=0`、`error_status=0x0`、I2C 无异常。

## 5. 根因排查建议

根因已定案为**适配器散热**,以下是验证与缓解:

1. **复现确认**:高功率档(本机满档 ~77W 入机)连续充电 10 分钟以上,触摸适配器
   外壳;若明显烫手且伴随 `ADAPTER_POWER_DERATE`(6560mA)出现 → 实锤热因。
2. **缓解**:给适配器留散热空间(不要压在床上/沙发缝里、不要盖东西);
   环境温度高时闪崩会更频繁,属正常保护行为。
3. **观测口**:复现时开着 §1 的采集,事后 grep
   `ADAPTER_POWER_DERATE|ap_exception_reason|adapter_temp` 即可定量;
   adapter_temp 爬到 850 后 ~1 分钟内必发生关机。
4. **排除市电的简易判据**:若日志再出现 `last=70`,先核对该时刻 vbus/ibat 是否
   有同步扰动——没有扰动即为采样伪影,不要据此怀疑插座。

## 6. 对 ExtremeGT 模块的启示

- 这类"闪崩→自动恢复"无需任何模块干预,watchdog 不应对此动作
  (会自动恢复,误触发反而打断重协商)。
- 若用户反馈"充电中掉功率",第一排查项是**适配器降额日志与市电环境**,
  而不是手机侧参数。
- 值得留意的观察口:`/sys/class/vfcs_class/`(状态机)、
  `dmesg | grep -E "ADAPTER_POWER_DERATE|ap_exception_reason|adapter_error_code2"`。
