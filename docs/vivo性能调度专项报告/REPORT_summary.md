# vivo 官方性能调度体系总览（V2505A / Android 17 / SM8850）

汇总自 9 个方向的专项报告（REPORT_*.md，均在本目录）。每条结论区分观察证据与推断。

## 一、总体分层

```
┌─────────────────────── 应用层 ───────────────────────┐
│ 游戏进程（无公开性能 SDK；白名单内游戏被注入 GameLib JNI 图形库）│
└──────────────┬───────────────────────┬───────────────┘
               │ GameWatchManager.execute (反射)      │ GameManager.isWhitelistApp / libgamemanager_aidl
┌──────────────▼───────────────────────▼───────────────┐
│              决策与执行层（system uid）               │
│ GameWatch (gamewatch_server Binder) 场景库 49,676 项  │
│ GameCube (游戏魔盒 UI/名单 5,131 条/模式下发)          │
│ gamespace (游戏空间入口)  GameKit (AI OCR 工具)        │
│ system_server (QoSUtils/AppStartPolicy/NewQoSUtils)   │
│ com.vivo.sps:rms (RmsService, mbd/ktop 只读消费者)     │
│ VPerf HAL (root, hal_vperf_default, sysfs_rsc 授权)   │
│ com.iqoo.powersaving (模式号 1004/1005/1011/1014)      │
└──────────────┬───────────────────────┬───────────────┘
               │ /sys/rsc (293 节点, sysfs)            │ /proc/fas/{render,monitor,scene,audio} (ioctl)
┌──────────────▼───────────────────────▼───────────────┐
│                   内核层（vivo_rsc 主控）              │
│ FAS 帧感知调度 / SVP VIP 线程 / QOS 带宽防饿死         │
│ ktop 内核 top / mbd refault / cpucap_notif / sched_boost│
│ 伴生: sched_walt + vivo_csc (CPU) | bwmon→qcom_dcvs   │
│ (DDR) | vivo_slowpath_opt (内存) | vivo_bgc (Binder)  │
│ symphony (文件预加载) | vivo_rms (meminfo) | vkyber (I/O)│
└──────────────────────────────────────────────────────┘
```

## 二、内核层组成

### vivo_rsc（主控模块）
- 1,368,064 字节，14 holder，`[permanent]` `(O)` out-of-tree 永久模块，scmversion `g41f446bcfc23-dirty`。
- 3,823 个命名符号 / 1,233 个 text 符号；前缀分布：`fas_` 354 text、`rsc_` 174、`svp_` 38、`qos_` 31、`vapt_` 41（VAPT=帧采样/渲染监控，见方向5）。
- 用户态 ABI：**sysfs `/sys/rsc` 共 293 文件** + procfs ioctl：`/proc/fas/{audio,scene,monitor,render}`、`/proc/vivo_rsc/frame_set_priority`（5 个 ioctl handler + 2 个导出扩展）。无独立 misc 设备节点。
- 186 个 `__ksymtab` 导出（25 个全局 text），含调度器/Binder/mutex/rwsem/futex vendor hooks。

### 子系统语义（方向8/9）
| 子系统 | 节点数 | 语义（推断依据见专项报告） |
|---|---|---|
| FAS | 160 | 帧感知调度。帧延迟/jank 反馈闭环 → uclamp + cpu freq QoS；`jerk_*` 严重掉帧独立路径；`fla_*` 滚动+非标准帧+启动加速；`adaptive_migrate_*` 任务迁移阈值；`fas_mem`→qcom_dcvs DDR 投票；`fas_gpu`→KGSL QoS。当前 `enable=0` 但 `fla_enable=1` |
| SVP | 48 | VIP/关键线程调度：选核、迁移、锁等待继承。当前 `svp_enable=0x3`，游戏相关开关（game_mode/game_pid_boost）未激活 |
| QOS | 17 | CFS 带宽防饿死：饿死等待阈值 5s，starving pull 15ms 周期，max_percent=20 |
| ktop | 8 | 内核线程级 top 数据源；读者=GameWatch、SPS/RMS、UEPP ActiveLoadReductionPlugin |
| mbd | 5 | 按 QoS 分级的 refault/allocstall 统计 |
| slowpath_opt | ~20 | 内存慢路径：直接回收/readahead/水位/页池/mmap 锁（归属 vivo_slowpath_opt 模块） |
| sched_boost | 1 | 写协议 `type tid level`，type0–9 共 10 种；type5–8 = 普通/持续 × 是否偏大核矩阵 |
| rsc_cgroup | 3 | `pid policy [flags]`；policy 1–8 = LB/B/SB/HB/LF/F/T/R |
| rsc_switch | 1 | 16 位功能位图，当前 `0xf391` |
| clone_boost | 1 | `clonePid cloneTid`，CPUAffinityPolicy 读后补设 type6 RT |

### 伴生模块链（方向6）
- CPU 调度主链：`sched_walt + vivo_csc + vivo_rsc`；`csc_driver` 另负责任务级 heatlimit。
- 带宽链：`bwmon → qcom_dcvs → DDR/LLCC/DDRQOS`；bwmon 引用 `vivo_fas_register_vote_thres`。
- `symphony`：文件预加载+页缓存分级（非 CPU 调度器）；`vkyber`：sda/sdb/sdc I/O 电梯；`vivo_bgc`：Binder 冻结/异步事务（非 compaction）；`vivo_rms`：meminfo 快速版本化读取；`pem_unisched`：CPU 频率 QoS 统计上报。

## 三、用户态组成

### GameWatch（com.vivo.gamewatch）——调度核心
- system uid、persistent；3 Service、2 Provider、0 Activity；运行时注册 `gamewatch_server` Binder（IGameWatch transaction 1–5）。
- **三条写 /sys/rsc 通道**：①直接 FileOutputStream；②`vivo_daemon.service` shell；③反射 VivoPerfManager → `vendor.vivoperfservice` Binder。
- 内置 SM8850 场景库：**49,676 个配置项 / 111 种 /sys/rsc 节点**（云端统一配置可覆盖基线）。场景按包名+sceneName+quality+ultraLoad+API+直播状态+温度选择（`useShellTemp` 时读壳温）。
- 数据源：SurfaceFlinger FPS 回调、帧直方图、`fas/runtime`、KTOP、3 秒周期温度、触控位图；socket `@resmon/@perfsdkmon/@netopt`；持有 `/proc/fas/monitor`。

### GameCube（com.vivo.gamecube）——游戏魔盒
- 游戏识别 = 厂商名单数据库（国内 5,131 条/海外 5,018）+ 已安装包校验 + 用户列表，非单纯 CATEGORY_GAME。
- 性能面板只下发模式号到 `com.iqoo.powersaving.POWER_MODE_CHANGE_SERVICE`（source=23）：均衡 1004 / 省电 1005 / Monster 1011 / Enhanced 1014。"按游戏自定义"只是记忆档位。
- 反射 `GameWatchManager.execute`（GameLib channel）请求功能并接收 `gamelibInfo` 广播；**不直接写 /sys/rsc**。
- gamespace 是入口：显式启动 `com.vivo.gamecube.GameCubeMainActivity`。

### 其他组件
- GameKit：签名权限保护的 AI 工具服务（截图 OCR/模型升级），**不是性能 SDK**。
- GameLib：宿主游戏进程内 Java/JNI 图形库（QV1 超分、MEMC 插帧、VNSS、录制），9 个 arm64 .so，RegisterNatives 私有注册；命令是图形类而非 CPU 调频。
- system_server：直接写 `qos_enable`（QoSUtils）、`launch`（AppStartPolicy.markUIThread）、`rsc_svp`（NewQoSUtils），最终走 `JniUtils.writeFile` native。持有 `sched_boost/rsc_cgroup/qos_state` 写 FD。
- surfaceflinger：持有 `/proc/fas/render` 两个 FD + `/dev/vivo_sync` + `fas/ex_boost` 写端 → **FAS 帧输入方**（二进制含 `FAS_PRESENT_FENCE`）。
- VPerf HAL（root, hal_vperf_default）：有 sysfs_rsc SELinux 授权，含 `svp/cpulimit`、`svp_ui_boost`、`launcher_boost_rt`、`qos/set_task_qos`。
- camera HAL（qcom + vivo camera3rd）：持有 `set_task_qos`、`rsc_svp` 写 FD。
- `libgui.so` VaptManager：引用 fas render/monitor、`notifyTargetFpsChange`。
- lmkd：只读 `system_heap/memory_info`；`com.vivo.sps:rms`：只读 `mbd/slowpath`、`mbd/refault`。

## 四、关键链路

1. **游戏帧性能**：surfaceflinger → `/proc/fas/render`(ioctl)+`ex_boost` → vivo_rsc FAS 闭环 → uclamp/cpufreq QoS/DDR/GPU 投票。
2. **游戏场景策略**：GameWatch（场景库）→ 三条通道 → `/sys/rsc/{rsc_boost,rsc_svp,rsc_switch,qos_state,imp_pid,cpulimit,gpu_reserve}`。
3. **UI 流畅度**：system_server（QoSUtils/AppStartPolicy）→ `qos_enable`/`launch`/`sched_boost`/`rsc_cgroup`；launcher 的 UXBoost-Thread/UiBoostThread 走 SVP。
4. **模式档位**：GameCube 面板 → powersaving 服务（1004/1005/1011/1014）→ 下游再映射到 fas/svp/qos 参数（映射表在 GameWatch 场景库，推测经 gamewatch_server）。
5. **内存 QoS**：vivo_slowpath_opt 按 RSC QoS 优化慢路径；mbd 统计供 `com.vivo.sps:rms` 消费。

## 五、仍未解决（unknowns）

- 1004/1005/1011/1014 → 具体 /sys/rsc 参数的运行时映射未做 Binder trace 验证。
- FAS ioctl request 数值与参数结构体布局未知；`fla_set_depend` 读拒绝。
- `sched_boost` type0–4/9 的精确首写事件、clone_boost 的 native 写入库未闭合。
- `mbd_enable`、`slowpath_opt`、`ktop_set` 的用户态配置者未知。
- GameLib 注入游戏进程的机制（Zygote hook? app 侧集成?）未确认。
- vivo_rsc 因 kptr_restrict=2 无符号地址，控制流级分析需拉 boot 分区提取 vmlinux。

## 专项报告索引
- 方向1 GameWatch：REPORT_GameWatch.md（含 gamewatch_configs/*.decoded.xml）
- 方向2 GameCube：REPORT_GameCube.md
- 方向3 GameKit/GameLib/gamespace：REPORT_3_GameKit_GameLib_gamespace.md
- 方向4 运行时拓扑：REPORT_runtime_topology.md
- 方向5 内核 vivo_rsc：REPORT_direction5_vivo_rsc.md（符号分类 vivo_rsc/）
- 方向6 伴生模块栈：REPORT_06_kernel_scheduler_stack.md
- 方向7 属性/init/框架层：REPORT_direction7_system_config.md
- 方向8 FAS：REPORT_8_FAS.md（fas_dump.txt 160 节点）
- 方向9 SVP/QOS/ktop/mbd：REPORT_direction9_svp_qos_ktop_mbd_slowpath.md
