# 方向 8：vivo FAS（帧感知调度）专项报告

## 1. 范围、时间与结论边界

- 设备：vivo V2505A / PD2505，Android 17，serial `10AFAB03FM003AB`。
- 节点采样时间：2026-11-06 16:58:49 CST；动态三次采样：17:00:41、17:00:43、17:00:45 CST。
- 构建指纹：`vivo/PD2505/PD2505:17/CP2A.260605.016_V000L1/261022204808:user/release-keys`，见 `analysis/device_pull/perf/fas_dump.txt:1-4`。
- 全程只读：仅执行 `find/cat/ls/grep/logcat/ps` 和读取 FD 链接；未写任何设备节点、属性或文件，未启动游戏或注入滑动。
- 标记约定：**【观察】**为命令输出、节点、符号、二进制字符串或 smali；**【推断】**为基于这些证据的解释；**【未知】**为当前证据不能确定的内容。
- 当前最关键限制：采样时 `fas/enable=0`，所以本报告不能把静态符号和历史计数当成“当前正在执行 FAS 控制”的证明。`fla_enable=1` 是另一个独立开关，不能与 `fas/enable` 混为一谈。

## 2. 交付物与证据索引

| 文件 | 内容 |
|---|---|
| `analysis/device_pull/perf/fas_dump.txt` | 160 个 `/sys/rsc/fas` 文件的完整单次读取结果 |
| `analysis/device_pull/perf/fas_dynamic_samples.txt` | 三次动态节点采样，验证静态场景下哪些计数变化 |
| `analysis/device_pull/perf/fas_kallsyms.txt` | `/proc/kallsyms` 中 `fas` 相关符号（含 `fast` 噪声） |
| `analysis/device_pull/perf/fas_proc_fds.txt` | `/proc/fas` 清单、SurfaceFlinger/GameWatch 实时 FD |
| `analysis/device_pull/perf/surfaceflinger.fas_strings.txt` | SurfaceFlinger 中 FAS、present fence、`/proc/fas/render` 字符串及偏移 |
| `analysis/device_pull/perf/libgui.fas_strings.txt` | libgui 中 VaptManager、`/proc/fas/render`、`/proc/fas/monitor` 字符串 |
| `analysis/device_pull/perf/fas_library_references.txt` | 对候选用户态库的定向字符串引用检查 |
| `analysis/device_pull/perf/gamewatch_fas_dex_context.txt` | GameWatch DEX 中 FAS 路径与监控字段上下文 |
| `analysis/device_pull/perf/fas_config_candidates.txt` | `/vendor/etc`、`/odm/etc`、`/system_ext/etc`、`/product/etc` 文件名搜索 |
| `analysis/device_pull/perf/fas_config_content_hits.txt` | 上述系统配置目录及 `/system/etc/uepp` 的内容搜索 |
| `analysis/device_pull/perf/fas_config_8850_summary.txt` | SM8850 GameWatch 云配置中选定 FAS 参数的值域和场景摘要 |
| `analysis/device_pull/perf/fas_logcat.txt`、`fas_tag_logcat.txt`、`fas_runtime_logcat.txt` | logcat 采样；未发现有效 FAS 业务日志 |
| `analysis/device_pull/perf/surfaceflinger`、`libgui.so` | 只读拉取的原二进制，供复核字符串偏移 |

本机 PATH 中没有 `jadx`、`jadx.bat`、`apktool` 命令；使用已有 apktool 解包结果、smali、Python ZIP/DEX 扫描与原始 ELF 字符串。REA 能识别 SurfaceFlinger 为 ARM64 ELF，SHA-256 为 `1e3df65198cd381ea2a43b224ad90d9cc8dd534c4a4053b66d22b86463efc3c6`，但当前 Windows/ARM64 主机没有可用的 Ghidra/Hopper/IDA/Kuna 提供器，故未声称获得原生反编译控制流。

## 3. 完整节点转储结果

### 3.1 覆盖情况

**【观察】**转储命令在设备端先执行 `find /sys/rsc/fas -type f | wc -l`，得到 160；本地用 `^===== /sys/rsc/fas/` 复核也为 160 个节标题。主节点、`param/*`、`runtime`、`frame_info`、`boost_info`、`power_telemetry`、`render_record`、`perfcnt`、`frame_sched`、`target`、`fla_*`、`fas_gpu/*`、`fas_mem/*` 均已包含。

命令（`MSYS_NO_PATHCONV=1` 已预先设置）：

```sh
adb -s 10AFAB03FM003AB shell "su -c 'for f in \$(find /sys/rsc/fas -type f | sort); do
  printf \"\n===== %s =====\n\" \"\$f\"; cat \"\$f\" 2>&1; printf \"\n\";
done'"
```

特殊读取结果：

- `fla_set_depend`：root 下仍为 `Permission denied`，原错误已保留在 `analysis/device_pull/perf/fas_dump.txt:388-390`。
- `target`：只有标题，内容为空，见 `analysis/device_pull/perf/fas_dump.txt:967-968`。
- `fas_mem/ddr_freq`：内容为空，见 `analysis/device_pull/perf/fas_dump.txt:228-230`。
- `frame_sched`：本次 show 输出在第 51 项中途结束，见 `analysis/device_pull/perf/fas_dump.txt:407-461`。这是内核节点本次返回的原文，不能据此声称只有 51 个内部对象。
- GameWatch 还引用 `fas_gc_uclamp`、`fas_gc_threads`、`fas_gc_freq_limit`，但这三个文件不在本机当前 160 个节点中；smali 对它们做存在性检查，见 `analysis/device_pull/perf/apktool/GameWatch/smali/s/c.smali:171-210`。应视为可选/其他版本接口，不能当成当前节点遗漏。

### 3.2 当前硬件拓扑和状态

**【观察】**

- 两个 CPU cluster：cluster 0 为 CPU 0–5、最大 capacity 762；cluster 1 为 CPU 6–7、最大 capacity 1024，见 `analysis/device_pull/perf/fas_dump.txt:19-82`。
- `enable=0`、`runtime="0 0"`、`target_fps=0`，见 `analysis/device_pull/perf/fas_dump.txt:204-206,935-936,963-965`。
- `adaptive=1`、`boost_enable=1`、`adaptive_boost_policy_version=2`，但 `adaptive_boost_pair=0`、`adaptive_boost_render=0`，见 `analysis/device_pull/perf/fas_dump.txt:475-477,591-612,883-885`。
- `adaptive_migrate=0`，render/pair 子开关和所有 migrate 阈值均为 0，`migrate_info` 为空，见 `analysis/device_pull/perf/fas_dump.txt:471-473,783-832`。
- `jerk_boost_enable=1`、`jerk_cpu_freq_adjust=1`、`jerk_gpu_freq_adjust=0`、`jerk_big_cpu_inc=2`、`jerk_mid_cpu_inc=2`、`jerk_win=100`，见 `analysis/device_pull/perf/fas_dump.txt:899-932`。
- `fla_enable=1`、`fla_min_cap_guarantee=800`、`fla_break_gap=400000`，见 `analysis/device_pull/perf/fas_dump.txt:260-265,345-346,379-381`。
- `fas_mem/enable=0`、`switch_type=0`、`vote_imm=0`、`vote_thres=0`、`vote_type=slowpath`、`fp_window_size=1`，见 `analysis/device_pull/perf/fas_dump.txt:228-257`。
- `fas_gpu/pm_qos` 为 `enable:0 max_gpuclk:0`，GPU util 记录为空，见 `analysis/device_pull/perf/fas_dump.txt:212-225`。

### 3.3 三次静态采样

**【观察】**17:00:41–17:00:45 的三次读取中：

- `frame_info` 始终为 total=201、smooth=107、jank=94、jerk_1=86、jerk_2=163；`boost_info` 不变；`total_jerk_2` 始终为 163。
- `runtime`、GPU util、GPU PM QoS、DDR vote 和 FLA scroll/frame 列表均不变。
- 只有 `cpu0_totalcycles` 至 `cpu7_totalcycles` 持续增长。

证据：`analysis/device_pull/perf/fas_dynamic_samples.txt:1-156`。

**【推断】**201 帧和 jerk 计数是 FAS 关闭后仍保留的历史/累计统计，不是本次静态采样新产生的事件；统计的起始 epoch 未知。PMU cycle 是独立的持续硬件计数，不能与 FAS 帧活动混用。

## 4. 配置文件发现

### 4.1 系统分区搜索（有界负结果）

执行了：

```sh
find /vendor/etc /odm/etc /system_ext/etc /product/etc -type f \
  \( -iname '*fas*' -o -iname '*perf*.json' -o -iname '*perf*.xml' \
     -o -iname '*game*.json' -o -iname '*game*.xml' \)

grep -R -n -I -E '(/sys/rsc/fas|adaptive_boost|adaptive_migrate|jerk_|fla_|target_fps|fas_mem)' \
  /vendor/etc /odm/etc /system_ext/etc /product/etc /system/etc/uepp
```

**【观察】**没有找到专用 FAS 配置文件。内容命中只有高通 KGSL 的 `dcvs_tunables/target_fps` 及其 perf 映射，例如 `/vendor/etc/init/hw/init.qti.kernel.rc:120`，不是 `/sys/rsc/fas/param/target_fps`，见 `analysis/device_pull/perf/fas_config_content_hits.txt:1-4`。

**【边界】**这是对上述目录和指定文本编码的搜索，不排除 APK 内、云配置、其他数据分区或二进制编码配置。

### 4.2 GameWatch SM8850 云配置是实际策略来源之一

**【观察】**GameWatch 的 `com.vivo.gamewatch.gamesdk.config.b.c()` 注册 `gw_control_8850.xml` 等配置，`f2/a` 使用 `content://com.vivo.abe.unifiedconfig.provider/configs` 并处理 `vge_configs.zip`，见：

- `analysis/device_pull/perf/apktool/GameWatch/smali/com/vivo/gamewatch/gamesdk/config/b.smali:65-124`
- `analysis/device_pull/perf/apktool/GameWatch/smali/f2/a.smali:49-55,126-163`

本地解码配置 `analysis/device_pull/perf/gamewatch_configs/gw_control_8850.decoded.xml` 给出直接策略：

- `common` 场景写 `fas/enable=0`，见该文件 `:4-9`。
- `commonGame` 场景先设置 target、hist、boost、jerk、migrate 等参数，最后写 `fas/enable=1`，见该文件 `:66-122`。
- `commonGame` 中 `target_fps=0` 与 `fas/enable=1` 同时存在，且 `adaptive_migrate=1`、render/pair migrate=1，见该文件 `:68,114-119`。

**【推断】**当前 `enable=0` 与普通/非活动游戏场景一致；GameWatch 云配置确实能改变 FAS 参数，不能把当前默认值当成游戏内生效值。

### 4.3 `frame_sched` 策略配置

**【观察】**`game_multiwin_config.decoded.xml` 中：

```xml
<fdcmd when="true"  fd="/sys/rsc/fas/frame_sched" fvalue="111" svalue="600 30 30 30 0 0 0" .../>
<fdcmd when="true"  fd="/sys/rsc/fas/frame_sched" fvalue="888" .../>
<fdcmd when="false" fd="/sys/rsc/fas/frame_sched" fvalue="222" svalue="600" .../>
<fdcmd when="false" fd="/sys/rsc/fas/frame_sched" fvalue="777" .../>
```

见 `analysis/device_pull/perf/gamewatch_configs/game_multiwin_config.decoded.xml:6-20`。GameWatch `c1/d.b()` 识别 `frame_sched`，并由 `c1/d.c()` 跟踪 777/888 对应状态，见 `analysis/device_pull/perf/apktool/GameWatch/smali/c1/d.smali:67-115,169-180,490-560`。

**【未知】**111/222/777/888 的内核命令完整定义和 `svalue` 各列单位尚未从内核控制流确认；不能仅凭数字自行命名。

## 5. FAS 输入与输出链路

### 5.1 帧边界和 FPS 元数据：SurfaceFlinger 是直接证据最强的生产者

**【观察：二进制】**SurfaceFlinger 含以下字符串及文件偏移：

- `0x52a1c: /sys/rsc/fas/ex_boost`，并有 `setFpInfo pid=%d appFps=%d limitFps=%d virtualFps=%d flags=...`，见 `analysis/device_pull/perf/surfaceflinger.fas_strings.txt:175-180`。
- `write FAS_PRESENT_FENCE`、`FAS_PRESENT_FENCE ioctl failed`，见同文件 `:205-209,241-245`。
- `writePoint open /proc/fas/render fail`、`/proc/fas/render`，见同文件 `:253-256,277-281`。

**【观察：实时 FD】**PID 2040 `surfaceflinger` 在采样时持有：

```text
17  -> /proc/fas/render
43  -> /proc/fas/render
54  -> /dev/vivo_sync
841 -> /sys/rsc/fas/ex_boost   （写端）
```

见 `analysis/device_pull/perf/fas_proc_fds.txt:15-20`。SELinux 也允许 SurfaceFlinger 搜索/读取 `proc_fas`、执行 0x6601–0x660b ioctl，并写 `sysfs_rsc`，见 `analysis/device_pull/perf/direction7/sepolicy_rsc.txt:145-149`。

**【观察：内核符号】**`vivo_rsc` 中有 `fas_render_open/show/ioctl`、`fas_presentfence_cb`、`fas_presentfence_notify`、`fas_gpu_queuebuf_notify`、`fas_sched_switch_tracer`、`fas_cpu_frequency_tracer`，见 `analysis/device_pull/perf/fas_kallsyms.txt:940-953,1251-1257,1500-1512,1653-1671`。

**【结论】**SurfaceFlinger 通过 `/proc/fas/render` 和 present-fence 路径向内核提供帧时点/渲染信息，并可写 `ex_boost`。这有直接二进制和实时 FD 证据，不是仅凭组件名推断。

### 5.2 libgui/VaptManager 是另一条原生输入路径

**【观察】**`/system/lib64/libgui.so` 含：

- `vaptDebug:VaptManager::init done mFasFd %d`
- `/proc/fas/render`
- `notifyTargetFpsChange:%s %d %d ;%d %d %d %d; %d %d`
- `/proc/fas/monitor`

见 `analysis/device_pull/perf/libgui.fas_strings.txt:25-36,43-52`。定向检查确认 libgui 引用 `/proc/fas/render` 和 `/proc/fas/monitor`，见 `analysis/device_pull/perf/fas_library_references.txt:5-9`。

**【推断】**libgui 内 vivo 的 VaptManager 参与 render/monitor FD 和 target-FPS 变化通知；它可能是应用侧帧信息与 FAS 的粘合层。**【未知】**尚未反编译其 ioctl 消息布局，也未找到名为 `libfrm.so` 的文件或 APK 中的 `libfrm` 字符串，因此不能把“libfrm”当作已证实组件。

### 5.3 GameWatch 主要是监控/策略配置，而非已证实的帧边界生产者

**【观察：实时 FD】**PID 5655 `com.vivo.gamewatch` 持有 `/proc/fas/monitor`，见 `analysis/device_pull/perf/fas_proc_fds.txt:21-23`。

**【观察：smali】**

- `r1/k.o()` 读取 `/sys/rsc/fas/param/adaptive_migrate` 并判断是否等于 1，日志 tag 为 `GameExceptionMonitor`，见 `analysis/device_pull/perf/apktool/GameWatch/smali/r1/k.smali:881-930`。
- `w1/f.b()` 读取 `/sys/rsc/fas/perfcnt`，解析 `total_jerk_2`、`ex_boost_cnt`、`sched_heavy_nr` 等，见 `analysis/device_pull/perf/apktool/GameWatch/smali/w1/f.smali:225-257,327-342,468-499`。
- `v1/r.e(Z)` 能构造 `echo <bool> > /sys/rsc/fas/param/debug`，见 `analysis/device_pull/perf/apktool/GameWatch/smali/v1/r.smali:1276-1316`。
- 云配置和 `c1/d` 处理 `frame_sched` 策略，见第 4.3 节。

**【推断】**GameWatch 的角色是读取 FAS 统计/状态、按场景下发生产配置、辅助调试；当前没有证据表明它是 SurfaceFlinger 那样的逐帧边界数据源。

### 5.4 内核内部关联信息

**【观察】**`deplist_waker` 的一个 RenderThread 依赖记录同时出现 `surfaceflinger`、`TimerDispatch`、应用进程和 RenderThread，见 `analysis/device_pull/perf/fas_dump.txt:131-143`。`frame_sched` 列表包含多个应用的 RenderThread，见同文件 `:407-461`。

**【推断】**FAS 不只统计单一应用线程，还结合调度 waking/switch、CPU frequency 和依赖链识别 render/pair 线程。具体依赖图构建规则仍需内核反编译验证。

### 5.5 输出：CPU、GPU、DDR

**【观察】**

- CPU：`fas_flp_set_task_uclamp`、`fas_flp_set_freq_limit_v2`、`fas_flp_qos_min_rqs/max_rqs`、`fla_freq_qos_req`；当前 FLA maxfreq plist 中可见 2361600/2438400 等记录，见 `analysis/device_pull/perf/fas_kallsyms.txt:960-972,1483-1486` 和 `analysis/device_pull/perf/fas_dump.txt:349-376`。
- GPU：`fas_gpu_fence_cb`、`fas_gpu_queuebuf_notify`、`fas_gpu_set_dev_constraints`、`fas_gpu_set_max_pwrlevel`，且 `msm_kgsl` 也导出 `fas_gpu_set_max_pwrlevel`，见 `analysis/device_pull/perf/fas_kallsyms.txt:341-342,1500-1523,1664-1671`。
- DDR：`fas_mem_queuebuf_notify`、`fas_presentfence_cb`、`cpufreq_map_mem`、`mem_timer_handler`；`qcom_dcvs` 导出 `vivo_fas_mem_freq_set/vivo_fas_mem_voter`，`bwmon` 有 `vivo_fas_vote_thres_set`，`memlat` 有 `vivo_fas_mem_freq_fp_adapt`，见 `analysis/device_pull/perf/fas_kallsyms.txt:818-823,1649-1653,1681-1682` 及 `analysis/device_pull/perf/kernel_stack/kallsyms_vivo_rsc.txt:3704-3732`。

**【推断】**FAS 的输出是 render/pair 任务的 uclamp、cluster frequency QoS、GPU pwrlevel/QoS 和 DDR 投票；FLA 的 QoS plist 是请求记录，不能仅凭节点文本断定该频率在采样瞬间实际生效。GPU 和 DDR 当前开关均为 0，未观察到实际投票。

### 5.6 logcat 结果

**【观察】**`fas_tag_logcat.txt` 为 0 行；`fas_logcat.txt` 和 `fas_runtime_logcat.txt` 中只有本次各方向 adb 命令自身包含 `fas` 的记录，没有有效的 FAS 业务 tag 或帧事件日志。

**【边界】**这只说明本次静态缓冲区没有可用日志，不能推出游戏场景或打开 debug 后也没有日志。

## 6. 参数语义推断

### 6.1 `target_fps`

**【观察】**当前为 0；SM8850 配置中不同场景值为 `0,45,48,60,72,83,90,93,120,144,165,185`，汇总见 `analysis/device_pull/perf/fas_config_8850_summary.txt:184-189`，具体 45/60/90/120/144/165 示例见 `gw_control_8850.decoded.xml:1725,3585,3905,3968,4720,9292`。libgui 有 `notifyTargetFpsChange`，SurfaceFlinger 有 app/limit/virtual FPS 字段。

**【推断】**它是场景期望/限制的帧率，不是实测 FPS。`commonGame` 中 0 与 `enable=1` 同时出现，因此 0 更可能表示自动、不覆盖或由显示/其他路径决定，而不是“关闭 FAS”。**【未知】**0 的精确定义和 144/165/185 是否经过显示能力裁剪，需看 store/ioctl 控制流。

### 6.2 `adaptive_boost_*`

**【观察】**`frame_info` 统计 smooth/jank/jerk，`boost_info` 分开统计 GPU/render/pair 的 long/fit/rundelay；内核有 `fas_flp_risk_eval`、`fas_flp_uclamp_inc_v2`、`fas_flp_adjust_jerk_freq_max`、`fas_flp_calculate_thread_cap_v2`。配置分别控制 render 和 pair 的 enable、threshold、jank_threshold，见 `analysis/device_pull/perf/fas_dump.txt:6-11,397-403` 和 `gw_control_8850.decoded.xml:107-112`。

**【推断】**这是以帧长、render/pair 延迟和 jank 历史为反馈的闭环增压，输出到任务 uclamp 和 CPU frequency QoS；`pair_filter/render_filter` 是过滤/平滑参数，`inc/dec`、`reset_count`、`celing`（源码拼写如此）控制增压积分/退阶和上限。

- `adaptive_hist_jank[_2/_3]`、`adaptive_hist_smooth`：历史窗口中不同 jank 档位及平滑权重/阈值；配置值域见 `analysis/device_pull/perf/fas_config_8850_summary.txt:61-84`。
- `adaptive_boost_v2_*_base_idx`、`*_max_limit`、`*_margin`：v2 策略的基准、限制和 render/pair 余量。**不能仅因名字含 `idx` 就认定是当前频率表下标**：配置中的 50/60 已超过当前 28/29 行频率表，可能需要映射或属于其他量纲。
- `adaptive_boost_pair/render`：两条独立控制路径；当前均为 0，但场景配置可设为 1。

**【未知】**threshold 的百分比/时间量纲、jank min/max 的比较方向、base_idx 的映射公式均未由反汇编确认。

### 6.3 `jerk_*`

**【观察】**`frame_info` 将 jerk_1/jerk_2 与 jank 分列，`perfcnt` 的 `total_jerk_2=163` 与 frame_info 相同；GameWatch 读取该字段并另有 `jerk_num`、`gpu_jerk_num` 监控字段。内核有独立 `jerk_loop`、`fas_flp_jerk_hrt_cb`、`fas_jerk_msg_wq`、`fas_flp_adjust_jerk_freq_max`。当前 CPU adjust=1、GPU adjust=0。

**【推断】**jerk 是比普通 jank 更严重的帧异常/长停顿处理路径，带独立定时器和工作队列，并可触发 CPU 频率修正；不是单纯由用户按钮触发的“瞬时冲刺”。`jerk_big/mid_cpu_inc` 很可能是 big/mid cluster 的频率档位增量，配置出现 2–15，见 `analysis/device_pull/perf/fas_config_8850_summary.txt:151-174`；`jerk_win` 是检测/抑制窗口，`jerk_boost_min/max` 是增压边界。

**【未知】**jerk_1、jerk_2、jerk_r 的严格分类阈值，`jerk_win=100` 的毫秒/帧单位，以及 inc 的精确索引公式均未确认。

### 6.4 `fla_*`

**【观察】**FLA 与主 FAS 开关独立，当前 `fla_enable=1`。符号和节点同时覆盖：

- scroll/frame：`fla_scroll_info`、`fla_support_non`、`rsc_create_scroll_info_locked`、`rsc_create_non_standard_frame`。
- 启动生命周期：`fla_comm_launch`、`fla_comm_launch_life_cycle`、`fla_launch_start`、`life_cycle_change_to_reboost`。
- 资源输出：`fla_min_cap_guarantee`、`fla_freq_lock`、`fla_freq_qos_req`、`rsc_minimum_cap_demand`、`send_freq_request`。

见 `analysis/device_pull/perf/kernel_stack/kallsyms_vivo_rsc.txt:3044-3144,3220-3314,3373-3465`。

**【推断】**FLA 是 FAS 树中覆盖滚动、非标准帧和应用启动生命周期的帧/启动保障子系统，不能简单缩写成“launch only”。`fla_min_cap_guarantee=800` 与 capacity 1024 量纲一致，表示最低容量保障；`fla_break_gap` 和 break 逻辑用于解除/中断频率保障。

**【未知】**FLA 缩写全称、`fla_break_gap=400000` 的时间单位、scroll 与 launch 共用状态机的细节未确认。SM8850 GameWatch 配置摘要中未找到 `fla_*` 或 `fas_mem` 项，未据此推断其配置来源。

### 6.5 `adaptive_migrate_*`

**【观察】**当前所有 migrate 开关和阈值均为 0，因此没有当前迁移行为可报告。`migrate_info` 表头按 `render big mid pair big mid` 分列；本机只有 cluster 0（CPU 0–5）和 cluster 1（CPU 6–7）。

**【观察：场景配置】**`commonGame` 可设 `adaptive_migrate=1`、render/pair=1。其他游戏场景出现：

- render/pair `threshold_freq_up=1400`、`threshold_freq_down=1300`、`threshold_time_down=70`，见 `gw_control_8850.decoded.xml:3570-3572,4184-4186`。
- `threshold_load` 可见 render=1450、pair=1100，也可见 commonGame=1，汇总见 `analysis/device_pull/perf/fas_config_8850_summary.txt:97-150`。

**【推断】**这是分别针对 render 和 pair 的 cluster 间迁移/回迁控制，以上升/下降频率阈值和 time_down 形成迟滞，load 决定迁移门槛。1300/1400 从数值上像 MHz，但仍是推断；`adaptive_migrate_cluster` 可能选择目标 cluster，`adaptive_migrate_swap` 可能交换 render/pair 角色或放置策略。

**【未知】**迁移方向、load 的归一化方式（1 与 1100/1450 显然不能按同一百分比直读）、time_down 单位，以及 big/mid 与本机两个 cluster 的精确映射都需要内核控制流或受控游戏场景验证。

### 6.6 `fas_mem`

**【推断：高可信】**它是帧路径驱动的 DDR 频率投票子系统：queuebuf/present fence 作为帧事件，经 `fas_mem_work`/`mem_timer_handler`，通过 `qcom_dcvs` 的 voter/freq_set 与 bwmon/memlat 联动。`vote_thres` 是投票阈值，`vote_imm` 是立即投票，`fp_window_size` 是 frame-present 窗口，`switch` 是路径模式，`vote_type=slowpath` 表示当前投票类型。

**【观察：当前无效】**enable=0、阈值为 0、ddr_freq 为空，故本次没有观察到实际 DDR vote，也没有证据证明 slowpath 是当前每次投票都经过的路径。

### 6.7 `fas_gpu`

**【推断：高可信】**GPU fence/queuebuf 提供 GPU 帧执行信息，`gpu_util` 保存序列号/时间记录；`pm_qos` 与 `set_max_pwrlevel/set_dev_constraints` 负责 GPU 频率上限或 QoS。当前 enable/util/max_gpuclk 全为 0，`adaptive_boost_gpu=0`、`jerk_gpu_freq_adjust=0`，所以未观察到动态 GPU 控制。

### 6.8 其他主要节点

- `frame_info`：smooth/normal/jank/jerk 等累计帧分类计数。
- `boost_info`：GPU/render/pair 长帧、fit 和 run-delay 统计。
- `cap_info`：render/pair 的 time/cycles/stall/instruction/freq 记录，本次为空。
- `perfcnt`：jerk、extra boost、sched heavy 和各 CPU PMU cycle。
- `power_telemetry`：本次为 6 个样本，CPU 两 cluster 约 601/599 mW、GPU 32 mW、total 1233 mW，见 `analysis/device_pull/perf/fas_dump.txt:954-956`；采样周期未知。
- `render_record`：pid/tid/pair/dep 记录，本次为空。
- `frame_sched`：frame-sched 健康状态及 render 列表；配置可控制，但当前 `enable0`。
- `deplist_*`：waker/wakee 依赖链及 audio/render 开关。

## 7. 当前值与游戏配置值对比

| 参数 | 当前静态值 | `commonGame` 配置 | 解释 |
|---|---:|---:|---|
| `fas/enable` | 0 | 1 | 当前不是活动游戏 FAS 状态 |
| `target_fps` | 0 | 0 | 0 可与 FAS 开启并存 |
| `adaptive_hist_jank` | 50 | 30 | 当前不是 commonGame 参数集 |
| `adaptive_boost_jank_min` | 1 | 0 | 同上 |
| `adaptive_boost_v2_mid_base_idx` | 20 | 50 | 同上 |
| `adaptive_boost_v2_big_base_idx` | 20 | 40 | 同上 |
| `jerk_boost_max` | 0 | 1 | 同上 |
| `adaptive_migrate` | 0 | 1 | 当前迁移关闭 |
| `adaptive_migrate_render/pair` | 0/0 | 1/1 | 当前 render/pair 迁移均关闭 |

证据：当前值见 `analysis/device_pull/perf/fas_dump.txt`；配置见 `analysis/device_pull/perf/gamewatch_configs/gw_control_8850.decoded.xml:66-122`。

## 8. 尚未解决的问题

1. **动态场景缺失**：未启动游戏、未写 enable，故没有游戏内 FAS 的实时帧输入、迁移、GPU/DDR 投票证据。
2. **内核控制流未反编译**：当前仅有 kallsyms 和节点文本，无法给 jerk 分类、migrate 单位、FLA 状态机、ioctl 消息布局下严格定义。
3. **libfrm 未证实**：未发现 `libfrm.so` 或相关字符串；已证实的是 SurfaceFlinger、libgui/VaptManager、GameWatch 三条用户态路径。
4. **配置来源不唯一**：系统 etc 目录未找到专用 FAS 文件，GameWatch 云配置是明确来源之一；FLA/fas_mem 的配置写入者仍未知。
5. **日志缺失**：当前缓冲区没有有效 FAS 业务日志，无法靠 logcat 重建一帧的完整时序。
6. **方向 4/5 交叉材料**：分析时尚未发现其正式 REPORT 文件；本报告使用并行产生的 `runtime_topology/fd_sys_rsc.txt` 对 SurfaceFlinger 的 `ex_boost` FD 做了交叉复核，未把未出现的方向结论当作证据。

## 9. 总结

**已证实**：FAS 树有 160 个文件；当前主 FAS、GPU、DDR、migrate 均关闭，FLA 独立开启。SurfaceFlinger 通过 `/proc/fas/render`、present fence 和 `ex_boost` 接入，libgui/VaptManager 提供 render/monitor/target-FPS 粘合，GameWatch 负责监控与场景策略。内核输出接口覆盖 uclamp、CPU/GPU QoS 和 DDR voter。

**高可信推断**：adaptive_boost 是帧延迟/jank 反馈闭环；jerk 是严重帧异常的独立快速处理路径；adaptive_migrate 是 render/pair 的 cluster 迁移迟滞控制；fas_mem 是帧事件驱动的 DDR 投票。

**不能过度解读**：target_fps=0 不是关闭；当前全 0 的 migrate 阈值不是游戏配置阈值；历史 201 帧不是本次静态采样新增；配置中的 1300/1400/70 和 base_idx 均不能在未反编译前赋予绝对单位。
