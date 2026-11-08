# iQOO15 快充 45°C 温度墙定位分析

目标：找到"电池温度 ≥45°C 强制关闭快充（fchg）"的判定逻辑，评估改到 48°C 的修改点。

分析对象（均从 git 历史 commit `bdb0bd1^` 的 `modules/` 恢复，vermagic 一致 = `6.12.23-android16-5-g801ede8ff3b9-4k`，同固件同源）：

- `out/iq15-vfcs-cms-core.ko`（= analysis/vfcs-cms-core.strings.txt 对应二进制，已验证 10432 条参考字符串全部命中）
- `out/vfcs-core.ko`、`out/vfcs-fchg-core.ko`、`out/vfcs-misc-core.ko`

## 结论（先说要点）

**四个模块里没有任何一处把"电池温度 45°C 关快充"写成硬编码立即数。** 全模块唯一的 `cmp w9, #0x1c2`(450) 在 `vfcs_cms_get_bat_heating_condition_status+0x1a8`，是低温电池加热逻辑，与关快充无关；vfcs-core 里 6 处 `mov w1, #0x1c2` 全是 `vfcs_fchg_get` 的属性 ID 实参，不是温度。

45°C 这个值只以 **DT 数据** 形式出现，有三处候选关卡。要改 48°C，改的是 **DT 表项（或其在内存里的解析副本）**，不是代码立即数。

> 2026-11 实机更新：X300 已坐实墙是关卡 A（HTCCC），且无需改数据——`cms_class/htccc_enable` 写 0 即可关闭，但必须**在触发前**关闭（触发后再写无效）。见文末「实机验证」。

## 候选关卡 A：cms scene-pro HTCCC（最可疑，DT 默认启用）

- 判定函数：`vfcs_cms_scene_pro_htccc_cb` @ `vfcs-cms-core.ko` .text+0x5c664（场景表 index 11，名字 "HTCCC"）
- 阈值来源：cms DT 节点 `vivo,scene-pro-data` 第 3 行（scene-pro-rc=[13,8]，行宽 8）：

  ```
  row3 = [1, 45, 55, 4100, 100, 1, 0, 0]
          en  t1  t2  vbat  dvbat use_vote
  ```

  （本行取自 analysis/cms-device-tree.json）
- 判据（精读反汇编得）：
  - `temp = [cms+0xc34c]/10`（0.1°C → 整°C；当 [cms+0x170]==3 时取 `max(temp, bat_therm1/10)`，bat_therm1 默认 350=35.0°C）
  - `vbat = [cms+0xc364]`（mV）
  - 激活条件：`vbat > 4100 && temp ≥ 45`（窗口上限 t2=55；temp>55 走另一迟滞分支）
  - 迟滞释放：`temp ≤ 45-2=43` 或 `vbat < 4100-100=4000`
- 生效路径：置 `htccc_status`（[cms+0xca3a]）=1，并在 scene votable（句柄在 [cms+0xc330]）上 `vote("HTCCC_EVENT", 1)`；该 votable 为 cms 场景投票汇总口（所有 `*_EVENT` client，含 PACKAGE_EVENT/NTC_HIGH_EVENT 等都在这类 votable 上投票），main_work 里读其 effective_result 决定电流/fchg。**HTCCC_EVENT 的最终动作没有 100% 坐实**（见"不确定项"）。
- 温度用的是**电池温度**（bat_therm 系），不是板温。

## 候选关卡 B：cms scene-chg PKG_PRO（DT 默认禁用）

- 判定函数：`vfcs_cms_scene_chg_pkg_pro_cb` @ .text+0x63690（场景表 index 31，名字 "PKG_PRO"）
- 阈值来源：cms DT 节点 `vivo,scene-chg-data` 第 9 行（scene-chg-rc=[16,6]，行宽 6）：

  ```
  row9 = [0, 400, 450, 350, 0, 0]
          en  thr_dchg_off thr_ibat0 thr_release
  ```

- 判据（`board_temp = [cms+0xc3c4]`，pkg 板温）：
  - `board_temp > content[2](450)` → vote(scene_votable, "PACKAGE_EVENT", 1)，日志 `pkg-pro, board_temp = %d > %d, scale ibat to 0`
  - 否则 `board_temp > content[1](400)` → 惰性 `find_votable("DCHG_DISABLE")` 到 [cms+0xc318]，vote("PACKAGE_EVENT", 1)，日志 `... disable fchg`
  - `board_temp ≤ content[3](350)` → 两路 vote 0，日志 `... remove restrictions`（有迟滞区间 350<t≤400 保持原状）
- **但 content[0]=0 → 函数开头直接走 "scene chg pkg_pro isn't enabled!" 返回，DT 默认关闭。** 除非用户态（glink/pkg_state sysfs）运行时下发新 content 启用它。
- DCHG_DISABLE votable 由 vfcs-core 创建（`vfcs_probe+0xcf4` create_votable，回调 `vfcs_fchg_disable_vote_callback`）→ 这才是真正关 fchg 的执行者之一。

## 候选关卡 C：vfcs-core 温度异常表（vfcs 节点 DT，本地未提取）

- 判定代码：`vfcs_check_temp_exception`（被内联进 `vfcs_loop_charge_protocol` @ vfcs-core.ko .text+0x10c40~0x11008；static 计数器符号 `vfcs_check_temp_exception.exception_count.0~7` 可证）
- 阈值来源：vfcs DT 节点 `vivo,temp-exception-thr`（8×s32，`vfcs_parse_reconfigurable_dt+0xe60` 解析；缺失时用 `_temp_exception_thr_default`(.data+0xf4) = `{700,700,700,650,650,1000,1000,0}`）
- 判定（值存于 vfcs 结构体，3 次连续超限才置位）：

  | idx | 温度源（[x19+off]） | 语义 | 默认 |
  |----|--------------------|------|------|
  | 0 | bat_board_temp (0x1440) | too high | 700 |
  | 1 | bat_btb_temp (0x144c) | too high | 700 |
  | 2 | bat_btb_temp_m (0x1454) | too high | 700 |
  | 3 | usb_conn_temp (0x145c) | too high | 650 |
  | 4 | usb_conn1_temp (0x1464) | too high | 650 |
  | 5 | adapter_temp (0x146c，与 adpt_temp_limit_thr 取小) | too high | 1000 |
  | 6 | adapter_conn_temp (0x1470) | too high | 1000 |
  | 7 | bat_temp (0x40c) | **too low**（≤ 触发） | 0 |

- 生效：置 [x19+0x1be8] 异常位（0x10000~0x80000000），日志 `xxx too high(%d),not allow fchg(thr=%d)`，主循环据此拒绝/退出 fchg。
- 注意：**这张表里没有"电池芯温度过高"项**，高温侧全是板温/BTB/接口/适配器温度。iQOO15 的 vivo,temp-exception-thr 实际值本地没有（cms-device-tree.json 只有 cms 节点），需要实机读 `/proc/device-tree/.../vfcs*/vivo,temp-exception-thr` 确认是否有人把某项设成 450。

## 已排除

- cms tc-data 温度-电流表：45°C 以上仍有非零电流（用户已确认），不是表封顶。
- "FCHG_TEMP"：只是场景表里的一个**名字**，其回调 `vfcs_cms_scene_pro_fchg_temp_cb` @.text+0x5e7e0 是 8 字节空桩（`ret`）；cms-core 里没有任何代码创建/投票名为 FCHG_TEMP 的 votable。名字有误导性，别追它。
- vfcs-fchg-core.ko：无温度判定逻辑（仅透传层）。
- vfcs_cms_check_fchg_current / fbon_cb：电流余量与 FBON_EVENT 投票，无 45°C 判据。

## 怎么定位"你设备上实际生效的是哪一路"

充电到 45°C 触发墙时抓 dmesg，按日志分流：

| 日志 | 对应关卡 |
|------|---------|
| `scene pro htccc data:(...)` / `htccc_status=1` | A：HTCCC |
| `pkg-pro, board_temp = %d > %d, disable fchg` | B：PKG_PRO（说明被用户态启用了） |
| `bat_board_temp too high...` / `bat_btb_temp_m too high...` / `usb_conn_temp too high...` | C：vfcs-core temp-exception |

也可以直接读 `/proc/device-tree` 里 cms 节点的 `scene-pro-data` 和 vfcs 节点的 `temp-exception-thr` 确认数值。

## 改到 48°C 的修改点

## 实机验证（X300 / V2509A，2026-11）

**已坐实：墙就是关卡 A（HTCCC）。**

- X300 的 `vfcs_cms` DT 节点里 `scene-pro-data` row3 = `[1,45,55,4100,100,1,0,0]`，默认启用（与 iQOO15 相同布局）；vfcs 节点没有 `temp-exception-thr` 属性，关卡 C 不存在。
- 关闭方法：写 `/sys/class/cms_class/htccc_enable` 为 0，运行时 scene-pro row3 的 `en` 即变为 0（dmesg 可见 `scene pro htccc data:(0 45 55 4100 100 1 0)`），无需改 DT、无需内存 patch。
- **关键行为：`htccc_enable` 只在触发前有效。** HTCCC 一旦触发（temp≥45°C 且 vbat>4100mV，状态已锁存/投票已生效），再写 0 不能解除已生效的限制；必须在充电升温前提前关闭。因此正确用法是常驻程序启动时关闭、退出时恢复（`src/fccc_turbo/fccc_turbo.c` 即此模式：启动写 0，所有退出路径恢复 1）。
- 附带观察：HTCCC 关闭后，44.2°C 时 USER_IBAT 的 binding vote 是 `INTELL_CHG_EVENT`（智能充电限流，实测 6200mA）而非 HTCCC——即 45°C 墙之下还有一层 intell chg 限流，抬高温度上限后如需更高电流要一并关注它。

### 若走 A（HTCCC）—— 数据修改，无代码 patch

- DT 层：cms 节点 `vivo,scene-pro-data` 第 3 行的 `45` → `48`（整°C 单位；t2=55 是窗口上限，迟滞释放点自动变为 46°C）。改 dtbo/dtb 重刷。
- 运行时层：probe 时 `vfcs_cms_of_get_rc_data_lv2` 把 scene-pro-data 拷贝到 kmemdup 数组，指针存 `[cms+0x150]`（`vfcs_cms_parse_func_dt+0x128` 附近解析）。内存 patch 即改该副本第 3 行第 2 个 int（数组偏移 `3*8*4 + 4 = 100` 字节处）45→48。

### 若走 C（temp-exception）

- DT 层：vfcs 节点 `vivo,temp-exception-thr` 对应项 450→480。
- 运行时层：解析后的 8-int 数组指针存 `[vfcs_struct+0x138]`（`vfcs_parse_reconfigurable_dt+0xea0` 处 `str x0,[x19,#0x138]`）。

### fbpat 风格校验特征（先全表校验一致再覆写）

- HTCCC 运行时 patch 的锚：scene-pro 副本共 13×8=104 个 s32，全表比对等于：

  ```
  1,0,0,0,0,0,0,0, 1,5,1,1000,0,0,0,0, 1,900,0,0,0,0,0,0,
  1,45,55,4100,100,1,0,0, 0,5,30,460,440,0,0,0, 0,1100,1547,500,0,0,0,0,
  1,260,670,620,570,520,0,0, 1,260,700,620,600,520,480,0,
  1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0,
  1,750,800,850,850,0,0,0, 0,0,0,0,0,0,0,0
  ```

  全等才把 row3 的 45 改成 48（`0x2d`→`0x30`）。注意这是**整°C**单位，不是 0.1°C。
- temp-exception 的锚：8 个 s32 全等实际读回值（实机先读一次确认）后再改目标项。

## 不确定项（明确标注）

1. ~~**HTCCC_EVENT 投票的最终动作未完全坐实**~~（已实机坐实，见上「实机验证」：X300 上 HTCCC 就是 45°C 墙，`htccc_enable=0` 可预防性解除）。iQOO15 上尚未复测。
2. **cms+0xc34c 温度字段的精确来源**未追到写入点（没匹配到对该偏移的直接 store，可能经寄存器偏移写入）；从被 /10 后与整°C 阈值比较、且与 bat_therm1 取 max 来看，是电池温度无疑。
3. **vfcs 节点 DT 未提取**：temp-exception-thr 在 iQOO15 上的实际值未知，不能排除"关卡 C 才是墙"（若值是 450）。
4. 二进制里存在无重定位的绝对地址访问（如 main_work+0x7b4 `mov w22,#0xc308; ldr x0,[x22]`、pkg_pro+0x154 `mov w8,#0xca40; ldr w8,[x8]`），说明这套 vfcs 模块按固定地址布局运行（或 .ko 被后处理过）。做运行时内存 patch 定位结构体时，可利用这一点：[绝对地址 0xc300]≈[cms+0xc300]，即 cms 主结构体在此构建中基址固定为 0xc000 附近——但此推断未经实机验证，谨慎使用。
5. HTCCC 的 temp 比较用的是**整°C**（阈值 45 对 temp/10），而 pkg-pro / temp-exception 用的是 0.1°C（450）。改的时候单位别搞错。
