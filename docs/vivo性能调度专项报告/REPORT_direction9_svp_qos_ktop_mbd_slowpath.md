# 方向9：SVP / QOS / ktop / mbd / slowpath_opt / sched_boost / clone_boost / rsc_cgroup

## 0. 范围、时间与证据等级

- 设备：vivo V2505A，serial `10AFAB03FM003AB`。
- 节点快照：2026-11-06 16:58:52–16:58:56 CST（设备 `date`）。节点是动态状态，以下“当前值”只指这一快照。
- 全程只对设备执行 `cat`、`ls`、`find`、`grep`、`/proc` 读取及 `adb pull`；未向设备节点、属性或文件写入。
- 本机无 `jadx`/`jadx.bat`，有 apktool 2.11.1。APK/JAR 用 `apktool d --no-res` 转为 smali；反编译证据是静态观察，不等于运行时执行。
- 【观察】指节点输出、FD、配置或 smali 直接内容；【推断】指据此作出的语义解释；【未知】指在本次搜索边界内没有可靠答案。

额外拉取的只读材料位于 `analysis/device_pull/perf/framework/`：

- `vivo-services.jar` SHA-256 `304411a948919e51985d601e819d1d2b1a2ec819c13d2f0fd6e0f784ce33a5df`
- `services.jar` SHA-256 `c79d8783d7af251235603275ab2da7c7f054601b801c2d9e53489cfdf1f59bc2`
- `SuperProcessSystem.apk` SHA-256 `87fb0941d88dbd2c0c1fc7e32e4a2b8e7cde59a5fdb6bf3434cec0587297124f`
- `GameWatch.apk` SHA-256 `f19877974481972f07d46e5b65b83731c4997b8bafa4a9766392b72d537362b0`

## 1. 完整节点转储清单

每个文件均含设备时间、目录权限、逐节点 `ls -l`、完整 `cat` 输出和 `CAT_RC`。

| 文件 | 节点数 | 内容 |
|---|---:|---|
| `analysis/device_pull/perf/svp_dump.txt` | 50 | `/sys/rsc/svp` 全部节点 |
| `analysis/device_pull/perf/qos_dump.txt` | 18 | `/sys/rsc/qos` 全部节点 |
| `analysis/device_pull/perf/ktop_dump.txt` | 6 | `ktop`、`ktop_fpid/latency/percent/set/task` |
| `analysis/device_pull/perf/mbd_dump.txt` | 5 | `/sys/rsc/mbd` 全部节点 |
| `analysis/device_pull/perf/slowpath_dump.txt` | 21 | `/sys/rsc/slowpath_opt` 递归全部节点 |
| `analysis/device_pull/perf/sched_boost_dump.txt` | 5 | `sched_boost`、`rsc_sched_boost_ctrl`、`rsc_boost`、`boost_count`、`appboost_late` |
| `analysis/device_pull/perf/clone_cgroup_dump.txt` | 7 | `clone_boost`、`rsc_cgroup(_dbg)`、`rsc_svp`、`rsc_dyn_svp_enable`、`rsc_support`、`rsc_switch` |

共 112 个节点。唯一读取失败是【观察】写-only 的 `slowpath_opt/memqos_page_pool/pgp_do_refill`，`CAT_RC=1 / Permission denied`，见 `slowpath_dump.txt:103-106`；其余节点均 `CAT_RC=0`。

覆盖限制：`qos/set_task_qos` 自称 `total: 85`，但 show 输出在第 36 项中间结束。本地复核该次 payload 为 4095 字节且末尾无换行，加 `CAT_RC` 前恰为内核一页 4096 字节，因此【观察】这是内核 show 页截断，不是主机丢失，也不能把文件中的任务行称为完整 85 项清单，见 `qos_dump.txt:124-163`。

通用采集形式（以 SVP 为例）：

```bash
export MSYS_NO_PATHCONV=1
"D:/Android/SDK/platform-tools/adb.exe" -s 10AFAB03FM003AB exec-out \
  "su -c 'for f in \$(find /sys/rsc/svp -type f | sort); do \
   echo ===== \$f =====; ls -l \$f; cat \$f 2>&1; echo CAT_RC=\$?; done'"
```

## 2. SVP

### 2.1 节点事实与语义

【观察】关键状态（均见 `svp_dump.txt`）：

- `svp_enable=0x3`，`svp_enable_mainthread=1`，顶层 `rsc_dyn_svp_enable=1`。
- `game_mode=0`，16 个模式位也全部为 0；`game_pid_boost=0`；`svp_ui_boost enable:0, pid:0`；`rt_ui_boost_to_prime enable:0`。
- `game_mode` 直接打印位名：
  - 0 `NEW_IDLE_BALANCE`、1 `FIND_ENERGY_CPU`、2 `CPU_EXCLUSIVE`、3 `FUTEX_OPT`
  - 4 `MTK_FLT`、5 `MTK_GAS`、6 `VIP_PREFER_IDLE`、7 `VIP_IGNORE_RT`
  - 8 `VIP_PENDING`、9 `VIP_GROUP_PREFER`、10 `VIP_GEAR_START_0`、11 `VIP_ONLY_MIDDLE`
  - 12 `LCPU_IDLEPULL`、13 `GC_BANDCPU`、14 `GC_BOOST_FREQ`、15 `RT_PREFFER_LCPU`
- `rsc_svp` 顶层列表有 9 项，含 `vivonrtservice`、camera provider 线程、launcher 的 `UXBoost/UiBoost/RecentTasksList`、surfaceflinger 线程，值见 `clone_cgroup_dump.txt:27-38`。
- `svp_stat` 同时统计 FUTEX/BINDER/RWSEM/MUTEX 等锁链、enqueue 和迁移，说明它并非只有“游戏帧率”一种作用。

【推断】SVP 是 vivo 的 VIP/关键线程调度优先级体系，覆盖选核、迁移、锁等待继承与 UI/游戏关键线程，而不是单纯“游戏模式开关”。这一解释直接受 `VIP_*` 位名、顶层 SVP 任务列表和锁统计支持。`svp_enable=0x3` 两个位的独立精确含义未由节点文本给出，不展开臆测。

【观察】`qos/in_gamematch=0x0`，不在 `svp` 目录而在 QOS 目录；`game_pid_boost=0`。本次五个游戏 APK、`vivo-services.jar`、`services.jar` 和 SPS 中均未找到这两个节点的直接字面写入，故其写入者与完整状态机【未知】。

### 2.2 `svp_ui_boost` 与 `rt_ui_boost_to_prime`

【观察】`vivo-services` 的 `com.vivo.services.qos.util.RTUtils`：

- `RTUtils.smali:17-19` 定义新路径 `/sys/rsc/sched_boost` 和旧路径 `/sys/rsc/svp/svp_ui_boost`。
- `RTUtils$ThreadRTRequest.smali:772-797` 在旧接口写入 `2*tid+1`（set）或 `2*tid`（cancel）。
- `RTUtils.smali:35-51` 将 type4 命名为 `RT_TYPE_UI_BOOST`。

【推断】`svp_ui_boost` 是旧版 UI RT boost 接口，type4 是其新 `sched_boost` 对应类型。

【观察】`rt_ui_boost_to_prime` 的直接写入者：

- `AppStartPolicy.smali:1569-1573` 写清理值 `0 7f 4 -1 -1 -1 -1`，`:1860-1862` 写动态构造值。
- `PreStartAppPolicy.smali:2004-2008` 有同样清理写，`:2335-2337` 写动态值。
- `CPUAffinityPolicy.smali:1064-1069` 也按 begin/end 写该节点。

【推断】它用于 app 启动/预启动及 CPU affinity 策略，将 RT UI 线程约束到 prime/大核；当前 `enable:0` 表示快照时未激活，不表示代码路径不存在。

### 2.3 SVP 用户态读写者证据

- 【观察·运行时】`vendor.qti.camera.provider-service_64`（PID 1923）持有 `rsc_svp` 写 FD 58，并同时持有多个 `qos/set_task_qos` 写 FD；见 `rsc_open_fd.txt:4-11`。
- 【观察·静态】GameWatch `s/c.smali:3288-3326` 的 `o(IZ)` 向 `rsc_svp` 写 `pid 1`/`pid 0`；`s/c.smali:1971-2009` 和 `:3202-3240` 还向 `rsc_boost` 写 PID/value。
- 【观察·静态】GameWatch 还直接写 `svp/imp_pid`（如 `p0/a.smali:444-600`）、`svp/cpulimit`（`o0/b.smali:159-161`）、`svp/gpu_reserve`（`b0/b.smali:3941-3943`）及 `svp/svp_limit_process`（`s/c.smali:3031-3040`）。
- 【观察·静态】`NewQoSUtils.oldSetThreadABinder` 在 `NewQoSUtils.smali:965-1052` 组装 tid/set 参数并写 `rsc_svp`，说明 system_server 的 PerfQoS/VBoost 路径也可写 SVP。
- 【观察·静态】SPS `com/vivo/rms/misc/service/e.smali:48-73` 写 `svp/cpulimit` 的 `2 -1 -1`。

因此 SVP 不是 GameWatch 独占：camera provider、system_server、GameWatch、SPS 均有不同节点的证据。`game_mode`、`game_pid_boost`、`in_gamematch` 的精确写入者仍未闭合。

## 3. QOS

### 3.1 带宽与饿死任务处理

【观察】`qos_dump.txt`：

- `qos_enable`：`rsc_qos_enable:0x1, rsc_cpu_qos_enable:0x1`；`qos_cpu_enable=0x1`。
- `qos_bw_enable=1`；`qos_rq_bandwidth` 为 `period:500 / quota:400 / penalty:100`。
- `boost_starving_cfstask`：`enable:0x1`，`trigger_boost_wait_delta:5000000000`。
- `pull_starving_task`：`enable:0x1`，`pull count:1086|78`，`check_period_ns:15000000`，`max_percent:20`，`skip_duration_ms:100`。
- `qos_task_running_timeout` 按 QoS 1–7 分别为 25/25/50/50/50/50/60（节点未在表中显式给单位，故报告不擅自补单位）。
- `qos_manual_bit` 给出 deplist/vendor min/mid/max 位：`16/32/64`，分别对应 QoS 5/6/7。
- 内核符号辅助证据存在 `rsc_check_starving_task`、`pull_starving_task_trigger`、`boost_starving_cfstask_enable`，见 `kernel_stack/kallsyms_vivo_rsc.txt:1480`、`:6079-6089`、`:7150`。

【推断】QOS 不仅给任务打等级，还在 runqueue 层实施带宽约束/惩罚，并对长期未获得 CPU 的 CFS 任务做主动迁移和提升。5,000,000,000 ns 即 5 秒的 boost 等待阈值，15 ms 是 pull 检查周期；带宽三项的单位未由帮助文本说明，保留原值。

### 3.2 QoS 任务视图与写入协议

【观察】`get_task_qos` 列出 `svp_static/dynamic/inherit`、`qos_static/dynamic/inherit/vendor/game`、locking promotion/depth、deplist/audio/mem 等字段，见 `qos_dump.txt:30-34`。`set_task_qos` 当前任务含 system_server、camera provider、GameWatch、`com.vivo.sps:rms` 等，证明这些子系统共享同一 QoS 状态面。

【观察】用户态协议：

- `NewQoSUtils.oldSetThreadQoS`（`NewQoSUtils.smali:1078-1176`）组装 `tid qos set 0`，交给 `JniUtils.setQoS`。
- `QoSUtils.setProcessQoS` 在 `QoSUtils.smali:2256-2314` 组装四元参数并调用 `JniUtils.setQoS`。
- GameWatch `s/c.H(I)`（`s/c.smali:2112-2135`）向 `qos/qos_state` 写 `pid 1 3`。
- SPS `p/f.smali:521-560` 向 `qos/set_task_qos` 写 `pid 1 0 0`。

【推断】QoS 有静态/动态/继承多维状态，用户态可以显式设置，也可由 binder/deplist 等关系继承。不同写路径有新旧协议差异，不能把 GameWatch 的 `qos_state` 三参数格式套到所有 `set_task_qos` 调用。

### 3.3 QOS 用户态写入者证据

- 【观察·运行时】camera provider PID 1923 持有 5 个 `set_task_qos` 写 FD；`vendor.vivo.hardware.camera3rd.provider@1.0-service` PID 1952 持有另一个写 FD，见 `rsc_open_fd.txt:4-14`。
- 【观察·运行时】system_server PID 3839 持有 `qos/qos_state` 写 FD 149，见 `rsc_open_fd.txt:18-22`。
- 【观察·静态】`services.jar` 的 `ThreadPriorityBooster` 检查 `set_task_qos` 并通过 `IVivoQosInjector.setThreadQoS` 委托，见 `ThreadPriorityBooster.smali:13`、`:264-294`、`:577-602`；`vivo-services` 的 `QoSUtils/NewQoSUtils` 是上述实际协议组装者。
- 【观察·静态】GameWatch 和 SPS 的直接写入见上节。
- 【未知】`qos_bw_enable`、`pull_starving_task`、`boost_starving_cfstask` 和 `in_gamematch` 的本轮搜索未发现直接用户态写入；其当前值可能来自内核默认或其他 native 组件，不能把“存在节点”当成写入者证据。

## 4. ktop

### 4.1 数据语义

【观察】`ktop_dump.txt:3-40` 的快照含：总 CPU usage、user/sys/hirq/sirq、10 秒 period、每核 usage/cap、32 个线程的 TIME/UTIME/STIME/PID/TGID/PPID/UID/bigcoreusg。`ktop_set` 显示 `ktop_enable:1 period:10000ms notify_percent:10`。

【观察】控制节点：

- `ktop_fpid`：16 个过滤 PID 槽，当前全 -1。
- `ktop_percent=10`。
- `ktop_latency=disable`。
- `ktop_task` 输出用法 `echo $pid > /sys/rsc/ktop_task`，未设置时为 `task 0 not found!`。
- `ktop_set` 用法列出订阅者 `RSC_ID_PERFD/BBKLOG/POWER/VIVOD/BIGDATA/SHELL/OTHER`，以及 `forcestart 1 period:10000`、`forcestop 0`，见 `ktop_dump.txt:54-65`。

【推断】ktop 是内核侧周期采样的“线程 top”数据源，不是仅读取 `/proc/stat` 的用户态统计；`ktop_set` 的 ID 是多个系统组件订阅/启停该数据源的协议。

### 4.2 谁读取 ktop

- 【观察】GameWatch `w1/b.a()`（`w1/b.smali:654-835`）直接读 `/sys/rsc/ktop`，解析总 CPU、时间、percpu 和线程表，且只保留前 10 个线程项；`o0/a.smali:273-323` 另有 `ExceptionThreadChecker` 读取路径。
- 【观察】SPS/RMS `t2/c.smali:23-31` 直接读 ktop，后续按 PID/TGID/线程名匹配并取 usage，类源名为 `CpuUsageCondition.java`。
- 【观察】UEPP 配置 `/system/etc/uepp/plugins/com.ueppp.misc.cpu.ActiveLoadReductionPlugin/default.xml` 声明 `path_ktop="/sys/rsc/ktop"`，本机证据汇总在 `rsc_rc_references.txt:28`。这证明第三个消费者配置存在，但未反编译该插件，具体算法不展开。
- 【未知】本轮未找到 `ktop_set` 的直接写入者。快照 `rsc_ktop_map:0` 但 `ktop_enable:1`，结合 `forcestart` 用法说明订阅位图不是唯一启动条件；确切启动者不能仅归因于 GameWatch。

## 5. mbd

### 5.1 不是“slowpath_opt”的同义词

【观察】`mbd_enable=1`。`refault_readable` 按 high/min/low QoS 分别输出 file/anon refault 计数；`slowpath_readable` 输出：

- high/min/low QoS 的 allocstall 阈值计数（如 high 3/7/16/33 ms，low 100/500/5000 ms）；
- 各 QoS allocstall 次数和 cost；
- `k*` 内核线程对应统计；
- dmabuf allocstall 统计。

详见 `mbd_dump.txt:12-75`。raw 与 readable 节点顺序读取时 lowqos anon refault 从 142732 变为 142741，符合活计数器继续增长，不是解析矛盾。

【推断】mbd 是内存“坏度/压力”观测面，核心指标是 QoS 分级 page refault 和分配慢路径 stall。`mbd` 的英文全称没有直接证据，报告不把 “memory badness” 当作已证实展开。它属于 `vivo_slowpath_opt` 内核模块，但与 `slowpath_opt` 控制节点是“统计面”和“控制面”的关系。

### 5.2 用户态消费者

- 【观察·运行时】`com.vivo.sps:rms` PID 5769 长期持有 `mbd/slowpath` 和 `mbd/refault` 只读 FD，见 `rsc_open_fd.txt:23-26`。
- 【观察·静态】SPS `com/vivo/rms/memory/e.smali:27-58` 用 `VfsReader` 读取 refault 并检查 `mbd_enable`；`memory/h.smali:27-58` 同样读取 slowpath；`memory/h.smali:167` 起解析 SlowpathData；`g/w0.smali:18-44` 还读取 `slowpath_readable`。
- 【未知】SPS 在这些路径中对 `mbd_enable` 是读取而非写入；本轮未找到 `mbd_enable` 的用户态写入者，不能将 RMS 标为 enable 的配置者。

## 6. slowpath_opt

### 6.1 控制面语义

【观察】`slowpath_dump.txt` 给出以下直接含义：

- 总开关 `slowpath_opt_enable=1`；`slowpath_opt_curr_state` 显示 QOS enable=1、各 QoS 当前 slowpath stall 任务数及系统任务分布。
- `memqos_wmark_enable=1`；low/mid/total 的 `slowpath_maxtsks` 分别为 8/2/32。
- `memqos_lock_mask=0x6`，帮助文本明确 `0x2:mmap read lock`、`0x4:mmap write lock`、`0x6:mmap read&write lock`。
- `memqos_pgp_enable=1`；movable/unmovable 页池上限均 2560，reclaimable 213；当前页数 2560/2560/213；`pgp_do_refill` 写-only。
- `memqos_wall_enable=1`，`memqos_wait_time=2000 ms`；帮助文本说明 atomic vendor hook 不能 schedule out 时会 force disable，当前 force=0。
- `folio_lock_owner_enable=1`，xa 节点显示 owner 记录、boost/restore/sync 计数；`amm_walk_enable=1`。
- debug 帮助文本包括 kswapd reclaim、memcg shrink 和 slowpath stall 自检，均原样保存在转储中。
- 内核符号进一步显示 `enable/disable_memqos_{pgp,lock,wmark,wall}`、`memqos_page_pool_alloc/refill`、`folio_lock_owner_boost_*`，见 `kernel_stack/kallsyms_vivo_slowpath_opt.txt:673-712`。

【推断】该子系统在内存分配/回收慢路径上组合使用：慢路径任务并发水位限制、按 QoS 等待/排队、mmap 锁优化、预分配页池和 folio owner 优先级继承。`amm_walk` 的缩写及具体算法无足够证据，保留未知。

### 6.2 用户态写入者

【未知】五个游戏 APK、`vivo-services.jar`、`services.jar`、SPS 和已搜索的 `/system/etc /system_ext/etc /product/etc /vendor/etc /odm/etc` 文本中，未发现 `slowpath_opt` 的配置写入者。RC 搜索只发现 SELinux `sysfs_rsc` 类型与 vperf 配置，未发现这些节点的 init `write/chown`。因此本轮不能归属其写入者；当前多数节点 mtime 为启动时间，可能为内核默认值，但这只是可能性。

## 7. sched_boost：实际 10 个编号、七个概念类别

### 7.1 节点与写协议

【观察】`sched_boost_dump.txt:3-43` 实际列出 type0–9，而不是只有七个编号：

| type | 节点名 | Java/策略对应与触发源结论 |
|---:|---|---|
| 0 | `input_boost` | `RT_TYPE_INPUT=0`；通用输入/UI RT 路径，精确首写事件未闭合 |
| 1 | `scroll_boost` | `RT_TYPE_SLIDE=1`；滑动/滚动 RT 路径，精确首写事件未闭合 |
| 2 | `fas_render_boost` | `RT_TYPE_RENDER=2`；render/FAS 渲染路径，未找到该编号的直接 Java 调用 |
| 3 | `launch_anim_boost` | `RT_TYPE_LAUNCHER=3`；launcher/启动动画类别，当前新策略多改用 type6 |
| 4 | `svp_ui_boost` | `RT_TYPE_UI_BOOST=4`；旧 `svp_ui_boost` 的新接口对应 |
| 5 | `vboost_policy_tl` | `RT_TYPE_NORMAL=5`；普通时限 VBoost RT |
| 6 | `vboost_policy_tl_b` | `RT_TYPE_NORMAL_BIG_CORE=6`；普通时限且偏 big/prime 核 |
| 7 | `vboost_policy_ntl` | `RT_TYPE_LONG=7`；长时限/持续 RT |
| 8 | `vboost_policy_ntl_b` | `RT_TYPE_LONG_BIG_CORE=8`；长时限且偏 big/prime 核 |
| 9 | `clone_boost_tl_b` | 仅内核节点名确认；Java `RTUtils` 公共校验仅到 8 |

【观察】`RTUtils$ThreadRTRequest.realSetThreadRT` 在 `RTUtils$ThreadRTRequest.smali:717-761` 对 `mOption=0` 直接写 `"%d %d %d"`，三个参数依次为 `type tid (set ? level : 0)`。`RTUtils.smali:1268-1276` 的完整公共参数是 `(set,type,level,option,timeout,pid,tidList)`。

【观察】本机新旧接口都存在，`SUPPORT_NEW_RT=true`。`RTUtils.smali:1458-1485` 选择 `realOption=0`，所以 type5–8 的公共策略调用进入上述 `sched_boost` 写路径；不要误以为调用处的 `option=1/2` 在本机一定仍写旧节点。

【观察·运行时】system_server PID 3839 持有 `sched_boost` 写 FD 349，`fdinfo flags:0400001` 即只写且 `O_LARGEFILE`，见 `rsc_open_fd.txt:18-22` 和采集的 fdinfo。此与 `RTUtils` 静态写路径互相印证。

### 7.2 可证实的策略触发源

【观察】静态调用汇总保存在 `analysis/device_pull/perf/sched_boost_callers.txt`（62 个调用点，含方法名、寄存器常量与 smali 行号）。典型例子：

- type5：`AppStartPolicy.onNavigationKeyBoost`（调用点 smali 2514）和 `onSlideUpSceneBoost`（2731/2764），`RTPolicy.notifyACoreScene`、`notifySetLauncherRT` 也使用普通 RT。
- type6：`AppStartPolicy.setAffinityAndRt`（1747/1767）、`setRenderThreadRT`（2946），`AppBoostPolicy.onLauncherIconAnimationBoost`（10860），以及 `DesktopPerformancePolicy.setSystemUIRenderThreadBoost`（187）。
- type7：`BootUpPolicy.lambda$bootCompleted$3`（354/779/1064）和 `setkcompactdRt`（1603），说明持续 RT 也用于 QoS 核心线程和内存维护线程。
- type8：`UIFirstPolicy.onLauncherTouchIconBoost`（1546），对 launcher touch icon 的 TID 设置长时限 big-core RT。
- clone 相关：`CPUAffinityPolicy.setThreadRT` 在目标 TID 不可用时读取 `clone_boost` 的 `clonePid cloneTid`，匹配 PID 后对 cloneTid 设置 type6，见 `CPUAffinityPolicy.smali:1718-1967`。

【推断】input/scroll/render/launch_anim/svp_ui 是早期或通用场景类型；type5–8 是当前 VBoost 的“普通/持续 × 是否偏大核”矩阵。当前 launcher 动画、SystemUI render 等场景不能机械映射到 type2/3，因为反编译显示其新策略实际使用 type6。type0–4 的精确首个写入事件、type9 的公共用户态 API 仍为【未知】；`rsc_sched_boost_ctrl=0x0` 的位协议也未闭合。

## 8. clone_boost

【观察】快照为 `10784 25122`，见 `clone_cgroup_dump.txt:3-5`；同次 ktop 将 TGID 10784 显示为 `om.phoenix.read`。

【观察】`CPUAffinityPolicy.smali:1859-1922` 将节点按两个整数解析为 `clonePid`、`cloneTid`，`:1952-1967` 在 clonePid 等于请求 PID 时对 cloneTid 调 `RTUtils.setThreadRT(type=6)`。内核符号另有 `show/store_launch_clone_boost` 与 `rsc_set_clone_boost`，`sched_boost` type9 名为 `clone_boost_tl_b`。

【推断】该节点用于在 native clone 创建执行线程后、Java 侧尚未得到稳定 TID 前，向 PerfQoS 暴露“进程 PID + 新执行 TID”，让 CPUAffinityPolicy 能补上 big-core RT/affinity；它不是 vivo“应用分身”管理开关。应用分身名称相似但语义不同。

【未知】本轮只找到 system_server 的读取与消费，未找到 `clone_boost` 的实际写入者（可能在 native clone 路径），因此“由哪一库写入”不能确认。

## 9. rsc_cgroup 协议

### 9.1 内核帮助与运行时状态

【观察】`rsc_cgroup` 原样帮助（`clone_cgroup_dump.txt:7-16`）：

```text
Set proc all tasks to cgroup
Usage: use 'write' systemcall to write buffer to this file.
buffer format: 'pid type [flags]'
Note: number of threads of proc will be returned by write!!!
Total task threads num excceed max(768) count 0
```

【观察】`rsc_cgroup_dbg`：init done，top group id=4、fg=2、bg=17、camera_daemon=11；fastcgroup=yes、debug=no，延迟工作参数 `20 (80ms)`。这些内核 group id 与下述用户态 policy 编号不是同一命名空间，不能直接一一等同。

【观察·运行时】system_server PID 3839 持有 `rsc_cgroup` 写 FD 1111，见 `rsc_open_fd.txt:18-22`。

### 9.2 Java/JNI 格式与 policy 名称

【观察】`JniUtils.smali`：

- `setProcessGroupByRsc(III)` 在 tid-kernel-policy 开启时写 `"%d %d %d"` = `pid policy flags`（`:28-83`），否则走 native `setProcessGroup`。
- `setThreadGroupByRsc(II)` 写 `tid policy 0`（`:97-150`）。
- 真正写节点是 private native `writeRscCGroup(String)`（`:156`）。

【观察】`CGroupConfigManager.policy2String`（`:2759-2833`）恢复用户态 policy：

| policy | 名称 | policy | 名称 |
|---:|---|---:|---|
| 1 | LB | 5 | LF |
| 2 | B | 6 | F |
| 3 | SB | 7 | T |
| 4 | HB | 8 | R |

【观察】`group2Policy`（`:2176-2216`）另将 RMS group 0/1/2/3/4 映射为 policy 2/8/6/7/7；这是 RMS 到写节点 policy 的转换，而非内核 group id 表。

### 9.3 flags 与调用者

【观察】`CGroupPolicy.smali:3027-3163`：

- 普通进程默认 flags=`0x10`；`isIgnoreFifoInGame` 分支可将其清 0，故 0x10 与 FIFO 策略相关【推断】，精确位名未确认。
- system_server 自身额外 `or 0xc`；system app 额外 `or 0x1`。
- 满足后台迁移配置时 `or 0x2`；虚拟显示或特定 game scene 分支 `or 0x40`。
- 同文件 `:1984` 将 SetProcessGroupThread 自身设为 policy 7（T）。

【推断】`rsc_cgroup` 是 PerfQoS 用来把进程全部线程或单线程纳入 vivo 调度组的协议，不只是标准 Android cpuctl 路径。0xc 的组合意义及全部 flags 位图【未知】，未把条件分支强行命名为内核常量。

## 10. 用户态证据总表

| 子系统 | 已确认读取者 | 已确认写入者/写路径 | 未闭合项 |
|---|---|---|---|
| SVP | GameWatch、PerfQoS | camera provider 的 `rsc_svp` FD；GameWatch 多节点直写；system_server `NewQoSUtils/RTUtils`；SPS cpulimit | game_mode、game_pid_boost、in_gamematch、部分 enable 位 |
| QOS | PerfQoS、GameWatch、RMS | 两个 camera provider 的 `set_task_qos` FD；system_server `qos_state` FD；GameWatch/SPS/vivo-services 静态写路径 | starving/bw 开关和 in_gamematch 写入者 |
| ktop | GameWatch、SPS/RMS、UEPP 插件配置 | 未发现 `ktop_set` 直接写入者 | forcestart 实际发起者 |
| mbd | `com.vivo.sps:rms` FD 与解析代码 | 未发现 `mbd_enable` 写入者 | enable 配置者 |
| slowpath_opt | 节点状态 | 本轮未发现用户态写入者 | 全部控制节点的配置来源、amm_walk 算法 |
| sched_boost | 节点与 PerfQoS dump | system_server FD + `RTUtils` 精确三参数写协议 | type0–4 精确首写、type9 公共 API、ctrl 位图 |
| clone_boost | `CPUAffinityPolicy` | 未发现实际写入者 | native 写入库及 type9 完整路径 |
| rsc_cgroup | `CGroupConfigManager` dump | system_server FD + `JniUtils/CGroupPolicy` | flags 全部位语义、内核 group id 完整映射 |

RC/SELinux 搜索原文在 `rsc_rc_references.txt`：未发现这些节点的 init chown/write；只发现 `/sys/rsc` 的 `sysfs_rsc` 类型、vperf 节点映射及 UEPP ktop 路径。节点权限以各自 dump 为准，不能用“system:system”反推具体进程。

## 11. 搜索边界与后续建议

1. type0–4、clone_boost 和 slowpath_opt 的最可能缺口在 native 库；后续可对 `libvivo_servers.so`、vperf client/provider、libmemperfd 及 camera provider 的链接库做符号/字符串交叉引用。此次后续 native 扫描时设备已不再出现在 `adb devices`，所以本报告不声称完成这些库的排除。
2. 若允许动态但只读的后续调查，可在不改节点的前提下用 fd 快照与现有日志关联写入进程；不能用主动 echo 试验来“验证协议”。
3. `qos/set_task_qos` 的 85 项完整清单需要内核侧分页/调试接口或改进 show 输出；重复 `cat` 仍只得到首页。
4. `mbd` 全称、QoS bandwidth 单位、sched_boost ctrl 位图、cgroup flags 全位图、amm_walk 算法必须保留为 unknown，除非取得内核源码或更直接的二进制证据。
