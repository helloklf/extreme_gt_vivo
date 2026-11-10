# iQOO 15(PD2505)充电相关芯片档案(bq27426 / SC8550V / 各料号辨析)

> 实测设备:iQOO 15(V2505A / PD2505,SM8850)。
> 用途:料号辨析存档——bq27426 是电量计不是电荷泵;iQOO 15 实际用料及实测参数。
> 日期:2026-11-10。

---

## 1. TI bq27426(被误传为"电荷泵"的料号,实为电量计)

来源:[TI 官方产品页](https://www.ti.com/product/BQ27426)(ACTIVE,资料 Rev.G 2023-04)。

| 项目 | 参数 |
|---|---|
| 功能 | 单节 Li-ion/Li-Po **系统侧电量计**(fuel gauge),不做功率转换 |
| 算法 | Impedance Track™ 阻抗跟踪,自动补偿老化/自放电/温度/倍率 |
| 预设化学曲线 | 4.2V / 4.35V / 4.4V 电芯三种预编程 profile |
| 上报量 | 剩余容量(mAh)、SOC(%)、电压(mV)、健康度 SOH |
| 接口 | I²C 400kHz,可配 SOC 中断 / 电池低电数字输出 |
| 电流采样 | 外接 10mΩ 采样电阻 |
| 功耗 | 睡眠 9µA(负载 <10mA 时) |
| 温度范围 | -40 ~ 85℃ |
| 封装 | DSBGA 9 球,1.62×1.58mm,NanoFree™ |
| Linux 驱动 | bq27xxx 主线驱动(BQ27XXXSW-LINUX) |

**辨析**:TI 的开关电容直充泵是 **BQ25970 / BQ25980** 系列;bq27xxx 全系是电量计。

## 2. iQOO 15 实际充电链路用料(实测)

| 角色 | 料号 | 证据 |
|---|---|---|
| 电量计(FG) | **中颖 SH366101**(非 bq27426) | 内核 `sh366101-0`,带学习容量(本机 1378/2371mAh,老化 ~58%)、`ex-fg` 共享数据 |
| 充电架构 | ADSP 外挂电量计方案 | 日志 `CHG_ARCH_ADSP_EXFG` |
| 直充电荷泵 ×2 | **南芯 SC8550V** 并联 | `vfcs-cp-0/1`、`vfcs_sc8550v_*`(日志 3653 次) |
| 泵备选料 | 南芯 SC8510、Maxim/ADI MAX77929 | `vfcs_sc8510`、`vfcs_max77929` 驱动(本机未绑定) |
| 无线 TRX | ConvenientPower **CPS4021**(30W 档) | 见[无线反充报告](iQOO15无线反充功率限制分析.md) |
| 笔充 TX(空载) | CPS8601 | 同上 |
| 适配器通信桥 | STM32 MCU | `/sys/class/stm32_debug/` 观测窗,见[适配器观测向导](iQOO15适配器观测向导.md) |
| 原装适配器 | 90W 档(`adapter_power=90`) | cms_class 节点 + 闪崩事件日志 |

### SC8550V 实测/配置参数

| 项目 | 值 | 出处 |
|---|---|---|
| 架构 | 2:1 开关电容电荷泵,双泵并联直充(fchg_ic_parallel_op) | 驱动 + DT |
| 单泵母线 OCP | 6000 mA | `vfcs_get_fchg_ic_state_info: ibus_ocp_thr_ma=6000` |
| 泵电压下限 | 3000 mV("used default 3v") | 同上 |
| 双泵并联切换点 | 1250 mA 母线 | DT `fchg-ic-switch-ibus-thr-ma` |
| 系统总能力 | 电池侧 17600 mA / 母线 8800 mA(常用 fcc 15600) | DT + `DCHG_FCC` votable |
| 直充退出 | heating-exit 2000mA、exit-fchg 1400mA;`DCHG_DISABLE` 20+ 投票者 | DT + 日志 |
| 实测满功率工况 | 入 9926mV×7802mA(~77W)→ 电池 ~15A@4.0V | 闪崩日志 t=9940~10724 |
| 寄存器接口 | I2C(`masked_write_reg reg=0x0C ...` 等) | 内核日志 |

## 3. 背景参考

- 闪崩事件中的泵行为:`hw_init` 重初始化、`set_vfcs_sm sm=2`、卸载/重载序列见
  [充电中后程功率闪崩分析](iQOO15充电中后程功率闪崩分析.md) §2。
- 泵的准入/退出门限全文见[低电量快充触发失败分析](iQOO15低电量快充触发失败分析.md) §1A。
