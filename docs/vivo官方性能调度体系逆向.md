# vivo 官方性能调度体系逆向全记录

> 设备：vivo V2505A（PD2505），Android 17 / SDK 37，SM8850 平台
> 内核：6.12.58-android16-6-gdd6fd5c98918-abogki543627742-4k，aarch64
> 固件指纹：vivo/PD2505/PD2505:17/CP2A.260605.016_V000L1/261022204808:user/release-keys
> 调查日期：2026-11-06；调查方式：全程只读（adb + KernelSU root），未写任何设备节点/属性/文件
> 证据归档：原始材料已于调查结束后清理（2.1GB 中间产物/APK/转储已删除）；9 份专项报告保留在 `docs/vivo性能调度专项报告/`，本文档为完整整合版

本文档整合 9 个方向的全部发现。每条结论区分【观察】（命令输出/反编译代码/符号的直接证据）、【推断】（由证据支持的解释）、【未知】。详细证据链见文末索引的专项报告。

---

## 一、体系总览

```
┌─────────────────────────── 应用层 ───────────────────────────┐
│ 游戏进程（无公开性能 SDK；白名单游戏被注入 GameLib JNI 图形库） │
└──────────────┬──────────────────────────────┬───────────────┘
               │ GameWatchManager.execute(反射) │ GameManager / libgamemanager_aidl.so (native)
┌──────────────▼──────────────────────────────▼───────────────┐
│                 决策与执行层（全部 system uid / root）          │
│                                                              │
│  GameWatch (com.vivo.gamewatch) —— 调度核心                   │
│    gamewatch_server Binder；49,676 项场景配置 / 111 种节点      │
│    三条写通道：①直接写 sysfs ②vivo_daemon.service shell        │
│    ③反射 VivoPerfManager → vendor.vivoperfservice             │
│                                                              │
│  GameCube (游戏魔盒 UI/名单库/模式下发)                          │
│  gamespace (游戏空间入口)   GameKit (AI OCR 工具，非性能 SDK)    │
│  system_server (vivo-services.jar: QoSUtils/AppStartPolicy/   │
│    NewQoSUtils/RTUtils/CGroupPolicy → JniUtils.writeFile)     │
│  com.vivo.sps:rms (RmsService；mbd/ktop 只读消费者)            │
│  VPerf HAL (root, hal_vperf_default, SELinux sysfs_rsc 授权)  │
│  com.iqoo.powersaving (模式号 1004/1005/1011/1014)             │
└──────────────┬──────────────────────────────┬───────────────┘
               │ /sys/rsc 293 节点 (sysfs)      │ /proc/fas/{render,monitor,scene,audio} (ioctl)
               │                              │ /proc/vivo_rsc/frame_set_priority
┌──────────────▼──────────────────────────────▼───────────────┐
│                      内核层（vivo_rsc 主控）                   │
│  FAS 帧感知调度 / FLA 启动与滚动加速 / SVP VIP 线程调度          │
│  QOS 带宽防饿死 / ktop 内核 top / cpucap_notif / VAPT 帧采样    │
│  sched_boost (type0-9) / rsc_cgroup / rsc_boost / rsc_switch  │
│                                                              │
│  伴生：sched_walt + vivo_csc (CPU) | bwmon→qcom_dcvs (DDR/LLCC)│
│  vivo_slowpath_opt (内存慢路径, 含 mbd) | vivo_bgc (Binder 冻结) │
│  symphony (文件预加载/页缓存) | vivo_rms (meminfo) | zcache     │
│  vkyber (I/O 电梯) | vivo_memdo (内存观测) | pem_unisched (遥测)│
└──────────────────────────────────────────────────────────────┘
```

---

## 二、内核层

### 2.1 vivo_rsc（主控模块）

【观察】
- `/proc/modules`：coresize=1,368,064 字节，引用数 14，`[permanent]`、`Live`、`(O)` out-of-tree，`scmversion=g41f446bcfc23-dirty`；模块参数 `enable_rsc=2 fastcgroup yes debug no`。
- 14 个 holder：dwc3_msm、msm_kgsl、bwmon、shrinker_proxy、bpfhelper、memlat、qcom_lpm、csc_driver、symphony、zcache、vivo_slowpath_opt、swapmem_free、sched_walt、vivo_csc —— 说明它向 WALT/cpufreq/GPU/DDR/CSC/slowpath 等提供内核内部接口。
- kallsyms 子集 7,791 行；去 mapping symbols 后 3,823 个命名符号，其中 text 符号 1,233 个。
- 186 个 `__ksymtab` 导出（25 个全局 text），含调度器、Binder、mutex/rwsem/percpu-rwsem/futex 的 vendor hooks 注册函数 —— 性能策略通过 GKI vendor hooks 嵌入调度器和锁慢路径。
- 内核线程：`fas_looper`、`fas_jerk_looper`、`rsc_ktop_noti`、`rsc_vivo_freq`、`vapt_looper`、`kworker/R-fas_ft`、`kworker/R-fas_mem_wq`、`kworker/R-fas_gpu_wq`。

【限制】`kptr_restrict=2`，root 下符号地址仍全为 0；文件系统中不存在 `vivo_rsc.ko`（/vendor、/system、/odm 均无），未拉 boot 分区。

### 2.2 用户态 ABI

【观察】三类接口，无独立 misc 字符设备：

1. **sysfs `/sys/rsc`**：共 293 个文件、15 个目录。归属：根目录 35 + fas 33 + fas/param 116 + fas/fas_gpu 3 + fas/fas_mem 8 + qos 18 + svp 50 属 `[vivo_rsc]`；`mbd`(5) 与 `slowpath_opt`(7+) 属 `[vivo_slowpath_opt]`；`system_heap/charge` 属 `[qcom_dma_heaps]`。
2. **procfs ioctl**：`/proc/fas/{audio,scene,monitor,render}`、`/proc/vivo_rsc/frame_set_priority`，共 5 个 ioctl handler + 2 个导出扩展（`dep_render_ioctl_ext`、`vapt_render_ioctl_ext`）。另有 `/proc/vivo_rsc/binder_boost_desc`（30 个 desc 槽）、`/proc/vivo_rsc/show_set_task_qos`（write-only）。
3. **VAPT procfs**：`/proc/vapt/{enable,target,render_record}` + 动态 per-pid 文件（state/fps/info/threshold/period/info2，当前未创建）。

【未知】ioctl request 数值、参数结构体布局、compat 行为。

### 2.3 权限模型

【观察】
- 节点 owner：286 个文件 `system:system`，7 个 `root:root`（debug、enabled、func_debug、fas/dep_init_count、fas/deplist_wakee、slowpath_opt/folio_lock_owner_enable、folio_lock_owner_xa）。
- mode 分布：0660×91、0664×151、0640×3、0444×9、0440×17、0220×1、0644×20、0200×1。
- SELinux：`genfscon` 将 `/rsc` 标为 `sysfs_rsc`，两个特殊节点为 `proc_frame_qos_list`、`proc_frame_set_priority_enable`。
- 有 `sysfs_rsc` 写授权的 domain：`hal_vperf_default`（完整读写）、`system_server`、`system_app`、`vivo_daemon`、`hal_camera3rd_default`、`hal_camera_default`、`hal_vibrator_default`、`init`；`atrace`、`dumpstate`、`surfaceflinger`、`vivonrtservice` 有特定 file 写权限。`lmkd`、`platform_app` 仅读。
- 注意：四个标准 init 目录（vendor/system_ext/system/odm 的 etc/init）grep `/sys/rsc` **0 命中**，节点权限来源（内核注册 or 其他启动代码）未确认。

### 2.4 内核子系统详解

#### FAS（帧感知调度）——160 节点

- 【观察】CPU 拓扑：cluster 0 = CPU 0-5（max capacity 762），cluster 1 = CPU 6-7（max 1024）。FAS 使用 capacity 域而非纯 kHz。
- 【观察】采样时静态值：`enable=0`、`runtime="0 0"`、`target_fps=0`、`adaptive=1`、`boost_enable=1`、`adaptive_boost_policy_version=2`、`adaptive_migrate=0`、`fas_mem/enable=0`、`fas_gpu pm_qos enable:0`、`jerk_boost_enable=1`、`fla_enable=1`、`fla_min_cap_guarantee=800`、`fla_break_gap=400000`。
- 【推断】`adaptive_boost_*`：以帧长、render/pair 延迟和 jank 历史为反馈的闭环增压，输出到任务 uclamp 和 CPU frequency QoS（符号：`fas_flp_calculate_thread_cap_v2`、`fas_flp_set_task_uclamp`、`fas_flp_set_freq_limit_v2`）。
- 【推断】`jerk_*`：比普通 jank 更严重的帧异常独立处理路径，带独立 hrtimer/workqueue（`jerk_loop`、`fas_jerk_msg_wq`），当前允许 CPU 调整、禁止 GPU 调整。
- 【推断】`fla_*`：覆盖滚动、非标准帧和应用启动生命周期的保障子系统（`fla_scroll_info`、`fla_comm_launch`、`fla_freq_lock`、`fla_freq_qos_req`），独立于 `fas/enable` 开关。不能缩写理解为"仅 launch"。
- 【推断】`adaptive_migrate_*`：render/pair 任务 cluster 间迁移/回迁迟滞控制（配置中 threshold_freq_up=1400、down=1300、time_down=70、load=1100/1450；单位未确认）。
- 【推断】`fas_mem`：帧事件驱动的 DDR 频率投票，经 `qcom_dcvs` 的 `vivo_fas_mem_freq_set/voter` 与 bwmon/memlat 联动。
- 【推断】`fas_gpu`：经 fence/queuebuf + KGSL QoS 控制 GPU（`fas_gpu_set_max_pwrlevel` 同时被 msm_kgsl 导出）。
- 【重要辨析】`fas/enable=0` 不代表 FAS 死了；`target_fps=0` 不是关闭 —— GameWatch 的 `commonGame` 场景中 `target_fps=0` 与 `fas/enable=1` 并存，0 更可能是"自动/不覆盖"。配置值域：0,45,48,60,72,83,90,93,120,144,165,185。
- 【帧输入方】**SurfaceFlinger 是已证实的逐帧输入方**：持有 `/proc/fas/render` 两个 FD + `/dev/vivo_sync` + `/sys/rsc/fas/ex_boost` 写端（pos=76）；二进制含 `FAS_PRESENT_FENCE` ioctl 字符串。`libgui.so` 的 VaptManager 是另一条粘合路径（render/monitor FD、`notifyTargetFpsChange`）。GameWatch 持 `/proc/fas/monitor`，是监控/策略方而非逐帧生产者。**未找到 libfrm.so，不能确认其存在**。

#### SVP（VIP/关键线程调度）——50 节点

- 【观察】`svp_enable=0x3`、`svp_enable_mainthread=1`、`rsc_dyn_svp_enable=1`；`game_mode=0`（16 个模式位全 0，位名：NEW_IDLE_BALANCE、FIND_ENERGY_CPU、CPU_EXCLUSIVE、FUTEX_OPT、MTK_FLT、MTK_GAS、VIP_PREFER_IDLE、VIP_IGNORE_RT、VIP_PENDING、VIP_GROUP_PREFER、VIP_GEAR_START_0、VIP_ONLY_MIDDLE、LCPU_IDLEPULL、GC_BANDCPU、GC_BOOST_FREQ、RT_PREFFER_LCPU）。
- 【推断】SVP 覆盖选核、迁移、锁等待继承与 UI/游戏关键线程（符号：`__do_svp_balance`、`pick_first_svp_task`、`binder_thread_set_svp_inherit`、`mutex_svp_inherit_enqueue`），不是单纯"游戏模式开关"。SVP 全称未确认。
- 【观察】`rsc_svp` 顶层列表含 vivonrtservice、camera provider 线程、launcher 的 UXBoost/UiBoost 线程、surfaceflinger 线程。
- 【观察】写入者（静态+运行时）：camera provider 持 `rsc_svp` 写 FD；GameWatch 直写 `rsc_svp`/`svp/imp_pid`/`svp/cpulimit`/`svp/gpu_reserve`/`svp/svp_limit_process`；system_server `NewQoSUtils.oldSetThreadABinder` 写 `rsc_svp`（`"%d %d 4"`）；SPS 写 `svp/cpulimit`（`2 -1 -1`）。`game_mode`/`game_pid_boost`/`in_gamematch` 的写入者【未知】。

#### QOS（CFS 带宽与防饿死）——18 节点

- 【观察】`qos_enable`：rsc_qos_enable=0x1、rsc_cpu_qos_enable=0x1；`qos_bw_enable=1`；`qos_rq_bandwidth` period:500/quota:400/penalty:100；`boost_starving_cfstask` trigger_boost_wait_delta=5,000,000,000ns（5 秒）；`pull_starving_task` check_period=15ms、max_percent=20、skip_duration=100ms。
- 【观察】`qos_manual_bit`：deplist/vendor min/mid/max = 16/32/64，对应 QoS 5/6/7；`qos_task_running_timeout` QoS 1–7 = 25/25/50/50/50/50/60。
- 【推断】QOS 给任务打静态/动态/继承多维等级，在 runqueue 层实施带宽约束/惩罚，对长期未获 CPU 的 CFS 任务主动迁移和提升（符号：`rsc_check_starving_task`、`pull_starving_task_trigger`、`qos_cpu_overutilized`）。
- 【观察】`get_task_qos` 自称 total:85，但 show 输出在 4096 字节内核页边界截断（第 36 项中途），不是完整清单。
- 【观察】写入者：两个相机 HAL 持 `set_task_qos` 写 FD（高通 HAL 5 个，pos 非零）；system_server 持 `qos_state` 写 FD（pos=1,554,023）；`services.jar` 的 `ThreadPriorityBooster` 经 `IVivoQosInjector` 委托 `QoSUtils/NewQoSUtils` 组装 `tid qos set 0` 协议。

#### ktop（内核线程级 top）——6 节点

- 【观察】`ktop_set`：ktop_enable:1、period:10000ms、notify_percent:10；订阅者 ID 协议：RSC_ID_PERFD/BBKLOG/POWER/VIVOD/BIGDATA/SHELL/OTHER，支持 forcestart/forcestop。
- 【观察】读者：GameWatch（`w1/b.a()`，只取前 10 线程项）、SPS/RMS（CpuUsageCondition）、UEPP 插件 ActiveLoadReductionPlugin（`/system/etc/uepp/plugins/.../default.xml` 声明 `path_ktop="/sys/rsc/ktop"`）。
- 【未知】`ktop_set` 的实际启动写入者。注意 `/system/lib64/libVivoDmService.so` 含采集命令 `cat /sys/rsc/ktop >> /data/bbklog/systemserver/wdog_ktop.txt`。

#### mbd（内存压力观测）——5 节点，属 vivo_slowpath_opt

- 【观察】`mbd_enable=1`；`refault_readable` 按 high/min/low QoS 输出 file/anon refault 计数；`slowpath_readable` 输出各 QoS allocstall 阈值计数（high 3/7/16/33ms，low 100/500/5000ms）、dmabuf allocstall。
- 【观察】消费者：`com.vivo.sps:rms` 长期持有 `mbd/slowpath`、`mbd/refault` 只读 FD，SPS 代码解析 SlowpathData。
- 【未知】`mbd_enable` 写入者；mbd 全称（"memory badness" 仅为猜测，报告不采用）。

#### slowpath_opt（内存慢路径优化）——21 节点，属 vivo_slowpath_opt

- 【观察】`slowpath_opt_enable=1`；`memqos_wmark_enable=1`，low/mid/total slowpath_maxtsks = 8/2/32；`memqos_lock_mask=0x6`（0x2=mmap read lock、0x4=mmap write lock）；`memqos_pgp_enable=1`，movable/unmovable 页池上限 2560、reclaimable 213（当前全满）；`memqos_wall_enable=1`，`memqos_wait_time=2000ms`；`folio_lock_owner_enable=1`；`amm_walk_enable=1`。
- 【推断】在内存分配/回收慢路径上组合：慢路径并发水位、按 QoS 等待/排队、mmap 锁优化、预分配页池、folio owner 优先级继承。
- 【未知】全部控制节点的用户态配置者（mtime 为启动时间，可能是内核默认值）。

#### sched_boost（RT boost）——实际 type0–9 共 10 种

| type | 节点名 | 语义（RTUtils 映射） |
|---:|---|---|
| 0 | input_boost | RT_TYPE_INPUT，通用输入/UI RT |
| 1 | scroll_boost | RT_TYPE_SLIDE，滑动/滚动 |
| 2 | fas_render_boost | RT_TYPE_RENDER，渲染/FAS |
| 3 | launch_anim_boost | RT_TYPE_LAUNCHER，启动动画（新策略多改用 type6） |
| 4 | svp_ui_boost | RT_TYPE_UI_BOOST，旧 svp_ui_boost 节点的新接口 |
| 5 | vboost_policy_tl | RT_TYPE_NORMAL，普通时限 |
| 6 | vboost_policy_tl_b | RT_TYPE_NORMAL_BIG_CORE，普通时限偏 big/prime 核 |
| 7 | vboost_policy_ntl | RT_TYPE_LONG，长时限/持续 |
| 8 | vboost_policy_ntl_b | RT_TYPE_LONG_BIG_CORE，长时限偏 big/prime 核 |
| 9 | clone_boost_tl_b | 仅内核节点名确认，无公共 Java API |

- 【观察】写协议 `type tid level`（`RTUtils$ThreadRTRequest.realSetThreadRT` 三参数）；本机新旧接口并存，`SUPPORT_NEW_RT=true`，type5–8 公共策略调用走 `sched_boost` 新路径。
- 【观察】system_server 持 `sched_boost` 写 FD（pos=25,142）。典型调用点：type5 导航键/上滑场景 boost；type6 launcher 图标动画、SystemUI render 线程；type7 开机完成、kcompactd RT；type8 launcher 触摸图标长时限 big-core RT。
- 【未知】type0–4 精确首写事件、type9 公共 API、`rsc_sched_boost_ctrl=0x0` 位协议。

#### clone_boost / rsc_cgroup / rsc_boost / rsc_switch

- `clone_boost`：【观察】格式 `clonePid cloneTid`（快照 `10784 25122`）；`CPUAffinityPolicy` 读后对 cloneTid 补设 type6 RT。【推断】用于 native clone 创建执行线程后、Java 侧拿到稳定 TID 前的 RT/affinity 补设，**不是应用分身开关**。【未知】实际写入者（疑在 native clone 路径）。
- `rsc_cgroup`：【观察】协议 `pid type [flags]`，写返回线程数；单线程形式 `tid policy 0`。用户态 policy 1–8 = LB/B/SB/HB/LF/F/T/R（`CGroupConfigManager.policy2String`）；flags：默认 0x10（疑与 FIFO 相关）、system_server 额外 or 0xc、system app or 0x1、后台迁移 or 0x2、虚拟显示/特定游戏 or 0x40。内核 group id（top=4/fg=2/bg=17/camera_daemon=11）是另一命名空间。system_server 持写 FD（pos=71,035）。
- `rsc_boost`：max value 255；GameWatch 硬编码协议：enter 先写 `rsc_switch=0x6` 再写 `pid 1`，exit 写 `rsc_switch=0x80000006` 再 `pid 0`。
- `rsc_switch`：16 位功能位图，当前 `0xf391`；位定义含 BINDER_LIMIT_NORMAL、APP_LAUNCH_BOOST、RSC_BINDER_BOOST_ENABLE、RSC_LOCK_BOOST_ENABLE、RSC_LOCK_BOOST_FUTEX_ENABLE、RSC_LOCK_BOOST_TO_BIGCORE 等；clear 用 `0x80000000|bit`。
- `launch`：【观察】`AppStartPolicy.markUIThread` 写 `"3000 <uid> <pid>"`；当前 `use fixed setting 1 boosttime: 300 ms (75)`。
- `cpucap_notif*`：CPU 频率限制调整通知记录（含调用栈，`cpu_frequency_limits_hook [vivo_rsc]`）。
- `frame_qos_list`：快照 `10081 1000 10102 10173`（UID 列表）。

### 2.5 伴生内核模块

| 模块 | 状态 | 角色（证据级别见专项报告） |
|---|---|---|
| sched_walt | 4,667 符号 | 高通 WALT 底座：ravg 负载跟踪、fair/RT 选核、迁移、core control、WALT governor；依赖 vivo_rsc、vivo_csc |
| vivo_csc | 1,460 符号 | CPU 频率/调度控制：PID 调节（kp=512/ki=16/kd=256/target=350）、场景投票、滚动（csc_fas_scroll）、heatlimit；控制面 `/sys/kernel/vivo_csc`。CSC 全称未确认 |
| csc_driver | 195 符号 | 任务级 heatlimit/action 执行器，与 vivo_csc 不是同一物；`/proc/csc/heatlimit` |
| bwmon | .ko 已拉取 | DDR/LLCC gold/prime 带宽测量 → DCVS 投票；depends 直接证明用 qcom_dcvs/vivo_rsc/sched_walt/MPAM，注册 `vivo_fas_register_vote_thres` |
| qcom_dcvs | 545 符号 | DDR/LLCC/DDRQOS 投票汇聚层；导出 `vivo_fas_mem_freq_set/voter` |
| symphony | 1,264 符号 | **应用文件预加载/页缓存分级/I-O 策略中枢（不是 CPU 调度器）**；prime/vital/fixed 页分级、F2FS/EROFS readahead、块请求紧急度；3 个 misc 设备 `/dev/symphony-{cmd,app-cmd,entry}` + `/proc/symphony/<pkg>/{entry,offset}` |
| vkyber_iosched | 346 符号 | sda/sdb/sdc 当前电梯（`[vkyber]`）；Kyber 风格延迟/token 改造版，依赖 symphony |
| vivo_rms | 24 符号 [permanent] | meminfo 快速/版本化读取（`/proc/meminfo_{version,quick}`）；加载依赖 symphony/zcache/thp_pool。RMS 全称未确认 |
| zcache | 511 符号 | 文件页/cleancache 压缩缓存（非 zram swap）；当前主开关 0、池为空 |
| vivo_slowpath_opt | 715 符号 | 按 RSC QoS 优化直接回收/readahead/水位/页池/mmap 锁（见 2.4 mbd/slowpath_opt） |
| vivo_bgc | .ko 已拉取 [permanent] | **Binder 冻结/阻塞/异步事务处理（不是 background compaction）**；`/sys/vivo_bgc/binder_cmd` 当前 `Binder Blocked Notify: STARTED` |
| vivo_memdo | 72 符号 [permanent] | kswapd/分配失败/温度内存观测；`/proc/vbmeminfo` |
| pem_unisched | 39 符号 | CPU 频率 QoS 统计与定时上报（遥测而非策略）；被 pie_driver 使用 |

模块依赖图（静态加载关系）：`bwmon→qcom_dcvs+vivo_rsc+sched_walt`；`sched_walt→vivo_rsc+vivo_csc`；`vivo_csc→vivo_rsc+qcom_pmu_lib`；`memlat→vivo_csc+vivo_rsc+qcom_dcvs`；`csc_driver→vivo_rsc`；`symphony→vivo_rsc`；`vkyber→symphony`；`vivo_rms→symphony+zcache+thp_pool`；`zcache→vivo_rsc`；`vivo_slowpath_opt→vivo_rsc`。

---

## 三、用户态组成

### 3.1 GameWatch（com.vivo.gamewatch）——调度核心

【观察】
- 身份：system sharedUserId、persistent、directBootAware；versionName 25.0.0.07（versionCode 250007）；3 Service、2 Provider、0 Activity、0 静态 Receiver。
- **真正的调度入口不在 Manifest**：Application 初始化时 `ServiceManager.addService("gamewatch_server", binder)`，接口 `com.vivo.gamewatch.common.IGameWatch`，transaction 1=gather、2=gathers、3=beginGather、4=endGather、5=execute。
- execute 分发命令：`turbo`、`turbo2`、`vaptTurbo`、`callback_register`、`sdkinfo_getter_register`、`vgt_server_register`、`notify_render_thread`、`notify_target_fps`、`notify_api_type`、`game_space`、`info_provide`、`control_panel`、`GameLib`、`net_opt`、`netcallback` 等。
- **三条 /sys/rsc 写通道**：①`FileOutputStream` 直写；②`vivo_daemon.service` 的 `runShell/runShellWithResult`（echo 命令）；③反射 `VivoPerfManager`（perfLockAcquire/perfHint/perfCommon/setPerfPath）→ `vendor.vivoperfservice`。
- 通用配置分发：`type="f"` 直写、`type="v"` 走 setPerfPath、缺省走 daemon shell —— **这解释了为什么 GameWatch 能写大量未硬编码的节点：节点和值主要由 XML 策略驱动**。

【观察】自带场景配置库（`assets/vge_configs_6154.zip` 内 `gw_control_8850.xml`，自定义编码：逐字节取反 + 按块内索引 i%8 循环左移，已离线解码）：
- SM8850 模型 41 个场景；**49,676 个 /sys/rsc 配置项、111 种节点**。
- `target_fps` 822 项，值集合 {0,45,48,60,72,83,90,93,120,144,165,185}；`qos/in_gamematch` 113 项（112 个为 1）；`svp/disable_input_boost` 924 项（897 个为 1）。
- `common` 默认场景：`fas/enable=0`、`qos_cpu_enable=1`、`in_gamematch=0`、`game_mode=0`、`imp_pid=-1 -1`。
- `commonGame`：`fas/enable=1`、`disable_input_boost=1`、`adaptive_migrate=1`、`useShellTemp=true`，按 temp=40/43... 分档。
- `svp_limit_process` 配置包名：com.kurogame.mingchao、com.tencent.tmgp.dfm、com.miHoYo.Yuanshen、com.miHoYo.GenshinImpact、com.miHoYo.hkrpg 等。
- 云端统一配置（`content://com.vivo.abe.unifiedconfig.provider/configs`，vge_configs.zip）可覆盖 APK 6154 基线，本机实际生效版本【未知】。

【观察】场景选择输入：包名 + sceneName + quality + ultraLoad + api + isLive + temp（>100 先除 100；useShellTemp 时读壳温 tshell_game/tshell_shell/primary_board_temp，3 秒周期）。

【观察】数据采集：SurfaceFlinger FPS 双向回调（transaction base+0x4e2a/0x4e2b，回调名 `sf_fps_monitor`）、帧直方图/boost 时间（BufferInfoItem）、`fas/runtime` 前台每 4 秒读取、`/sys/rsc/ktop`、`fas/perfcnt`、触控位图（`/sys/touchscreen/ts_super_node`，仅统计用）、整局统计 collector（battery/frame/network/traffic/temperature/latency）。

【观察】带外通道：三个 local socket `@resmon`（池 4）、`@perfsdkmon`（池 25）、`@netopt`（池 1），JSON 协议，按 peer pid/uid 映射包名 —— 游戏内 SDK/native 侧上报场景、渲染线程和网络数据的通道。另有 12 组动态广播过滤器（SCREEN_ON/OFF、BATTERY_CHANGED、包增删换、相机开关、统一配置更新完成等）。

【观察】硬编码节点写入（触发条件详见 REPORT_GameWatch.md 第 5 节）：`rsc_boost`（前后台 enter/exit）、`rsc_svp`（`tid 1/0`）、`qos/qos_state`（`<id> 1 3`）、`svp/imp_pid`（FrameAware 选两个进程/线程，含 miHoYo 冷启动特例）、`svp/cpulimit`（AppBoost 两组值）、`svp/gpu_reserve`、`svp/svp_limit_process`、`fas/frame_sched`（多窗口 111/222/777/888 协议）、`/proc/sys/walt/rsc_load_enable`、`afs/gw_hint`（GC begin/end）、`fas/fas_gc_threads`、`trace_on_status`、初始化时 `/sys/rsc/vgarph/*` 图形特性发布（MEMC/超分/异步动画/v_sync_opt/光追）。

【辨析】
- GameWatch 里的 `sched_boost` 字符串只是 perfLock 名称，不是写 sysfs 节点。
- `vboost` 是 ServiceManager 服务名（SDK≥36 用 vboost，旧版用 rms），不是节点。
- GameWatch 未发现 mbd 直接引用（有界负搜索）。

### 3.2 GameCube（com.vivo.gamecube）——游戏魔盒

【观察】
- 85MB、4 个 DEX、93 个组件（60 activity/15 service/6 receiver/12 provider）、**无打包 .so**（GPU profile 等经外部 `profilemanager-jni`）；system sharedUserId、coreApp。
- **游戏识别 = 厂商名单数据库 + 已安装包校验 + 用户添加/删除**，不是 CATEGORY_GAME（DEX 全量搜索 0 命中）：`GameCube.xml` 国内名单 5,131 条/5,124 包名，`GameCubeOversea.xml` 海外 5,018 包名；统一配置版本更高时覆盖。`FunctionConfigList.xml`（49 个功能名单、2,777 项）做第二轮硬件/渠道/包名过滤。
- **性能面板只下发模式号**到 `com.iqoo.powersaving` 的 `POWER_MODE_CHANGE_SERVICE`（intent action，`source=23`）：

| 档位 | ModeType | command |
|---|---|---:|
| 均衡 NORMAL | 1 | 1004 |
| 省电 POWER_SAVING | 2 | 1005 |
| Monster | 5 | 1011 |
| Enhanced/BOOST | 6 | 1014 |
| Monster Plus / Boost Plus | 7/8 | 默认 1014，条件分支 1011 |

- "按游戏自定义"只是 `key_mode_in_game_<pkg>` 记忆档位，不是独立调度档；旧 `game_cube_gpu_mode=3` 才是自定义 GPU Profile（QTI ProfileManager → nativeSaveProfile，参数为 MaxSamples/TextureMaxAniso/TextureFilteringQuality/MipLodBias，是图形参数不是频率）。
- 旧 `competition_mode` 是免打扰/后台通话/亮度/导航条组合开关，不能与 Monster 1011 混同。
- 调用关系：GameCube →（反射 `GameWatchManager.execute`）→ GameWatch（**GameWatch 是被调方**）；GameWatch →（定向广播 `com.vivo.gamewatch.game.{quality,targetfps,gamelibInfo}`）→ GameCube。
- 侧边栏链路：性能入口 → PerformancePanelManager → ModeShiftBar → presenter → POWER_MODE_CHANGE_SERVICE。GameWatch 的 targetfps/quality 广播可提示用户切档（引导，非直接内核请求）。
- **GameCube 完整 DEX 对 /sys/rsc、vivo_rsc、svp/qos/ktop/mbd 0 命中** —— 不直接写调度节点。

### 3.3 GameKit / GameLib / gamespace

- **GameKit**（com.vivo.gamekit）：【观察】签名权限 `REQUEST_GAMETOOLS` 保护的 **AI 工具服务**，AIDL `IGameTool` 只有 `sendScreenCaptureSync/Async`（截图 OCR/图像识别，SharedMemory 传图）和 `requestUpdate`（模型升级）。**不是性能 SDK**。GameCube 声明了权限并携带 AIDL，但 smali 未找到实际绑定调用点（当前版本是否使用【未知】）。
- **GameLib**（com.vivo.gamelib）：【观察】无 Android 导出组件的**进程内 Java/JNI 图形库**：QV1 超分、MEMC 插帧、VNSS、游戏录制/截图。9 个 arm64 .so（libVSR 11MB、libVkLayer_VSR 12MB、libvivogamecore_qv1_gles/vulkan、libMEMCSDK、libVNSSSDK 等）；`GameLib_jni` 仅导出 JNI_OnLoad，经 RegisterNatives 注册 21 个私有 native 方法。两个 Manager 反射 `GameWatchManager.execute` 转发**图形状态命令**（`ecosdk_setMessenger`、`disable_gpu_memc`、`all_surface_ready`、`set_rotation`、`vnss_capture` 等），4 个图形 .so 直接 NEEDED `libgamemanager_aidl.so`（native → 系统 GameManager 的 AIDL 通道）。白名单经 `GameManager.isWhitelistApp`（原神/星铁/鸣潮/燕云等有低分辨率特参）。**注入机制（谁把 GameLib 加载进游戏进程）【未知】**。
- **gamespace**（com.vivo.gamespace，组件包名 com.vivo.game）：【观察】游戏空间 UI 入口 —— 本地游戏筛选（排除 com.android/com.vivo/com.bbk 前缀，quickgamecenter/minigamecenter 例外）、Room 数据库、使用统计；首页"魔盒入口"显式启动 `com.vivo.gamecube.GameCubeMainActivity`（验证"gamespace 是入口、GameCube 是功能实现包"）。只读 `/sys/class/thermal`，不写 /sys/rsc。

### 3.4 system_server 与 framework 层

【观察】`/system/framework/vivo-services.jar` 直接写节点（经 native `JniUtils.writeFile`，实现库未定位）：

| 类.方法 | 写入内容 | 节点 |
|---|---|---|
| QoSUtils.setEnableQoS | "1"/"0" | /sys/rsc/qos/qos_enable |
| AppStartPolicy.markUIThread | "3000 uid pid" | /sys/rsc/launch |
| NewQoSUtils.oldSetThreadABinder | "%d %d 4" | /sys/rsc/rsc_svp |
| QoSUtils.enableRscDepList | 参数 | /sys/rsc/fas/deplist_waker |
| QoSUtils.writeFrameQosList | UID 列表 | /sys/rsc/frame_qos_list |
| RTUtils$ThreadRTRequest.realSetThreadRT | "type tid level" | /sys/rsc/sched_boost（+旧 svp/svp_ui_boost） |
| AppStartPolicy/PreStartAppPolicy/CPUAffinityPolicy | 清理值/动态值 | /sys/rsc/svp/rt_ui_boost_to_prime |
| JniUtils.setProcessGroupByRsc | "pid policy flags" | /sys/rsc/rsc_cgroup |

【观察】应用侧 API 链：`vivo-framework.jar` 的 `VPerformance` → `ServiceManager.getService("vendor.vivoperfservice")` → `IVivoPerfManager` Binder（**VPerformance 是客户端，不直接写 sysfs**）。

【观察】静态引用但当前不存在的节点（视为条件/旧版路径）：`/sys/rsc/fas/fla_echo_break_freq_enable`、`fas/resume_cpus`、`fla_boost_uclamp`、`rsc_gpu_freq_break`、`qos/qos_ni_enable`、`qos/qos_stat`、`svp/support_vboostsdk`、`binder_cmd/binder_event/binder_frozen_callback`。

### 3.5 vendor 服务与 native 库

【观察】进程与身份：

| 进程 | 用户 | SELinux domain | 角色 |
|---|---|---|---|
| /vendor/bin/hw/vendor.qti.hardware.perf2-hal-service | root | — | 高通 perf2 HAL |
| /vendor/bin/hw/vendor.vivo.hardware.vperf-service-aidl | root | hal_vperf_default | **vivo VPerf HAL，有 sysfs_rsc 完整读写授权**；二进制含 `/sys/rsc/svp/{cpulimit,svp_ui_boost,launcher_boost_rt}`、`qos/set_task_qos`、`persist.vendor.vperf_launch_thermal` |
| /system/bin/vivoperfservice | system | vivoperfservice | vendor.vivoperfservice Binder 宿主；→ VPerf AIDL 的代理跳【未闭合】 |
| /system_ext/bin/perfservice | system | vendor_perfservice | 高通 perfservice |
| /system/bin/vivonrtservice | system | — | 转场动画服务（`vivo_nrt_service`），SVP 任务列表成员 |
| /system/bin/vivosymphonyservice | root | — | symphony 用户态 |
| /system/bin/vspflinger | root | — | — |
| /vendor/bin/irsc_util | — | — | init.svc.irsc_util=stopped |

【观察】携带 /sys/rsc 路径的 native 库：`libvivo.vas.qos.ctrl.so`（rsc_svp、set_task_qos）、`libvivo.vaf.system.so`（set_task_qos）、`libaudioclient.so`（set_task_qos）、camera/vcf 系列库（set_task_qos、rsc_svp）、`libvcf_platform_utils.so`（system_heap/charge、rsc_svp、kswapd 相关）、`libvivo_raw_nr_cpu.so`（system_heap/memory_info）、`libVivoDmService.so`（ktop 采集命令）。

【观察】系统属性（当前值 ↔ 默认来源）：
- `/vendor/build.prop`：`ro.vendor.extension_library=libqti-perfd-client.so`、`ro.vendor.perf-hal.ver=3.0`、`ro.vendor.perf.scroll_opt=1`（代码 fallback=false，镜像覆盖）、`vendor.perf.framepacing.enable=1`、`vendor.perf.qfps.enable=1`。
- 来源未知：`persist.vendor.vperf_launch_thermal=true`、`persist.vendor.vperf_sched_schedstats=1`、`persist.sys.perfdata.scene_switch=2`（只控制 big data 上报，非 rsc 开关）、`persist.sys.scxrsc.flag=0`、`sys.vivo.tip_switch_perfmode=1-2`、`sys.init.perf_lsm_hooks=1`。

---

## 四、运行时拓扑（2026-11-06 17:00 快照）

### 4.1 /sys/rsc 直接打开者（精确扫描 /proc/*/fd，仅 6 个进程）

| 进程 | 节点 | 模式 | fd pos |
|---|---|---|---|
| system_server (3839) | qos/qos_state、sched_boost、rsc_cgroup | 写 | 1,554,023 / 25,142 / 71,035（非零=此前很可能写过） |
| surfaceflinger (2040) | fas/ex_boost | 写 | 76 |
| vendor.qti.camera.provider (1923) | qos/set_task_qos ×5、rsc_svp | 写 | 多个非零 |
| vivo camera3rd HAL (1952) | qos/set_task_qos | 写 | 0 |
| lmkd (1011) | system_heap/memory_info | 读 | 0 |
| com.vivo.sps:rms (5769) | mbd/slowpath、mbd/refault | 读 | 0 |

【注意】GameWatch、vperf、vivonrtservice、deformer、upslide **在该快照中没有直接 /sys/rsc fd** —— GameWatch 的写入是"开-写-关"短生命周期（fd 快照抓不到），且可经 daemon shell / VivoPerfManager 间接写入。`grep rsc` 的宽泛扫描会误中 `/proc/vivo_rsc/frame_set_priority`（deformer、upslide 持有），必须与 `/sys/rsc` 区分。

### 4.2 Binder 服务注册表（相关部分）

`gamewatch_server` [IGameWatch]、`game_manager` [IGameManager]、`qos` [IQoS]、`vboost` [IVBoost]、`rms` [IRM]、`rms_statistics` [IGather]、`sps` [ISpServer]、`performance_hint` [IHintManager]、`vendor.vivoperfservice` [IVivoPerfManager]、`vendor.perfservice` [IPerfManager]、`vendor.qti.hardware.perf2.IPerf/default`、`vendor.vivo.hardware.vperf.IVPerf/default`、`vendor.vivosymphonyservice` [IVivoSymPhonyManager]、`vivo_nrt_service` [ITransitionService]。

### 4.3 已建立的绑定

- GameWatch → `RmsService`（com.vivo.sps:rms）、`PemService`（com.pemPieProcess）、`MessageDisplayService`（com.vivo.abe）—— requested/received/hasBound 均 true。
- socket 监听：GameWatch @resmon/@perfsdkmon/@netopt；com.vivo.sps:rms @rms_gc_listen。

### 4.4 典型场景数据流（综合各方向，实线=已证实，虚线=推断）

```
[游戏帧性能]
游戏渲染 → SurfaceFlinger ──/proc/fas/render ioctl + present fence──→ vivo_rsc FAS
         → libgui VaptManager ──/proc/fas/{render,monitor}──→ vivo_rsc
  vivo_rsc FAS 闭环 ──→ 任务 uclamp / CPU freq QoS（sched_walt/vivo_csc）
                    ──→ GPU pwrlevel/QoS（msm_kgsl）
                    ──→ DDR 投票（fas_mem → qcom_dcvs ← bwmon）

[游戏场景策略]
GameWatch 场景库（包名+画质+温度+...选场景）──→ 三通道写 /sys/rsc
  {rsc_boost, rsc_svp, qos_state, imp_pid, cpulimit, gpu_reserve, frame_sched...}

[UI 流畅度]
system_server (QoSUtils/AppStartPolicy/RTUtils/CGroupPolicy)
  ──→ qos_enable / launch / sched_boost / rsc_cgroup / frame_qos_list
launcher UXBoost/UiBoost 线程 ──→ SVP 列表

[模式档位]
GameCube 面板 ──command 1004/1005/1011/1014, source=23──→ com.iqoo.powersaving
  ──下游映射到 fas/svp/qos 参数（疑经 gamewatch_server 场景库）【未闭合】

[内存 QoS]
RSC QoS ──→ vivo_slowpath_opt（慢路径优化）──→ mbd 统计 ──→ com.vivo.sps:rms 消费
lmkd ──→ system_heap/memory_info（读）
```

---

## 五、对你观察 /sys/rsc 困惑点的解释

1. **`fas/enable=0` 不代表 FAS 死了**：`fla_enable=1` 独立工作；游戏场景（commonGame）才会置 `enable=1`；`target_fps=0` 与 enable=1 并存，0 是"自动"。
2. **静态值看不出因果关系**：`in_gamematch`/`game_mode`/`game_pid_boost` 平时全 0，游戏档位经 GameWatch 49,676 项场景库中转，且云端统一配置可覆盖 APK 基线。
3. **`sched_boost` 字符串歧义**：GameWatch 代码里它是 perfLock 名称；sysfs 节点写协议是 `type tid level`（type0-9）。
4. **fd 快照抓不到 GameWatch**：它是开-写-关短写入 + daemon shell + VivoPerfManager 三条间接通道；长期持 fd 的是 system_server/surfaceflinger/相机 HAL。
5. **/sys/rsc 是共享根目录**：mbd、slowpath_opt 属 vivo_slowpath_opt，system_heap/charge 属 qcom_dma_heaps，不能全算到 vivo_rsc。
6. **`qos/set_task_qos` 读不全**：内核 show 在 4096 字节页边界截断（自称 85 项只见 36 项）。
7. **宽泛 `grep rsc` 误报**：会命中 `/proc/vivo_rsc/frame_set_priority` 等无关路径。

---

## 六、未解决问题（unknowns）汇总

| # | 问题 | 需要的下一步 |
|---|---|---|
| 1 | 模式号 1004/1005/1011/1014 → 具体 rsc 参数的运行时映射 | 授权后做 Binder trace / 受控游戏场景抓节点变化 |
| 2 | FAS ioctl request 数值与参数结构体布局 | 拉 boot 分区提取 vmlinux（vivo_rsc 内联），匹配 build-id 后反汇编 |
| 3 | vivo_rsc 真实符号地址 | kptr_restrict=2；需 vmlinux 静态分析 |
| 4 | sched_boost type0–4/9 精确首写事件；clone_boost native 写入库 | 扫 libvivo_servers.so、vperf client 库符号 |
| 5 | mbd_enable / slowpath_opt / ktop_set 的用户态配置者 | 扩大 native 库扫描（设备掉线前未完成） |
| 6 | GameLib 注入游戏进程的机制（Zygote hook？） | 分析 com.vivo.game.GameManager framework 实现 |
| 7 | GameCube 当前是否实际绑定 GameKit | 运行时观察 |
| 8 | vivoperfservice → vendor VPerf AIDL 代理跳 | native 指令级或 Binder trace |
| 9 | `JniUtils.writeFile` 的 native 实现库 | 扫 libandroid_servers.so RegisterNatives |
| 10 | 云端统一配置（vge_configs.zip）当前生效版本 | 查 /data 下 abe unifiedconfig 数据 |
| 11 | `qos/set_task_qos` 85 项完整清单 | 内核分页接口或 debugfs |
| 12 | SVP/VAPT/MBD/CSC/RMS/BGC 缩写全称 | 内核源码或更多字符串证据 |
| 13 | rsc_cgroup flags 全位图、内核 group id 完整映射 | 内核控制流分析 |
| 14 | adaptive_migrate 单位与迁移方向、jerk 分类阈值、fla 状态机 | 受控游戏场景 + 内核反汇编 |

---

## 七、专项报告索引

10 份报告保留在 `docs/vivo性能调度专项报告/`：

| 报告 | 内容 |
|---|---|
| REPORT_summary.md | 精简总览 |
| REPORT_GameWatch.md | GameWatch 逆向（422 行，含三条写通道、49,676 项配置统计、IPC） |
| REPORT_GameCube.md | GameCube 逆向（名单库、模式下发、调用链） |
| REPORT_3_GameKit_GameLib_gamespace.md | 三包角色定位 |
| REPORT_runtime_topology.md | 运行时进程/fd/socket/Binder 拓扑 |
| REPORT_direction5_vivo_rsc.md | vivo_rsc 符号分类与接口全景 |
| REPORT_06_kernel_scheduler_stack.md | 伴生内核模块栈 |
| REPORT_direction7_system_config.md | 属性/init/SELinux/framework 配置链 |
| REPORT_8_FAS.md | FAS 专项（160 节点转储摘要） |
| REPORT_direction9_svp_qos_ktop_mbd_slowpath.md | SVP/QOS/ktop/mbd/slowpath 专项（112 节点转储摘要） |

原始证据（APK、apktool 输出、dexdump、节点全转储、kallsyms、拉取的 .ko/jar/二进制）已清理；需要复核时可重新从设备拉取（复现要点：Git Bash 下 adb 设备路径需 `export MSYS_NO_PATHCONV=1`；root 为 `su -c`，KernelSU `u:r:ksu:s0`；设备无 jadx，反编译用 apktool 2.11.1 `C:/Users/hello/apktool/apktool.jar` + SDK dexdump + NDK llvm 工具链）。
