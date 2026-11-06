# 方向 5：vivo_rsc 内核模块静态分析与接口全景

- 设备：vivo V2505A，serial `10AFAB03FM003AB`
- 内核：`6.12.58-android16-6-gdd6fd5c98918-abogki543627742-4k`，aarch64
- 分析日期：2026-11-06
- 边界：只读分析；未写设备节点、属性或文件，未加载/卸载模块，未拉取或改写 boot 分区。
- 证据目录：`analysis/device_pull/perf/vivo_rsc/`

## 0. 结论摘要

1. **【观察】`vivo_rsc` 是永久驻留的 out-of-tree 模块。** `/proc/modules` 给出 `coresize=1368064`、引用数 14、`[permanent]`、`Live`、`(O)`；`scmversion=g41f446bcfc23-dirty`。它出现在 `/proc/modules`，所以本报告不把“内联于 vmlinux”当作已证实事实；其实际加载来源仍未确定。
2. **【观察】已获得完整 `[vivo_rsc]` kallsyms 子集：7,791 行。** 去除 `$d`/`$x` 等 mapping symbols 后有 3,823 个命名符号，其中 `t/T` 文本符号 1,233 个。
3. **【观察】root 下地址仍全部为 0。** 设备 `kptr_restrict=2`，`/proc/kallsyms`、`/proc/modules` 和 `/sys/module/vivo_rsc/sections/*` 都没有泄露真实地址；本报告没有用符号顺序伪造地址或反汇编。
4. **【观察】未发现独立 rsc/fas/svp/qos/ktop/mbd misc 字符设备。** ioctl 主要暴露在 **procfs**：`/proc/fas/{audio,scene,monitor,render}` 与 `/proc/vivo_rsc/frame_set_priority`。
5. **【观察】`/sys/rsc` 是共享根目录，不全是 `vivo_rsc` 的节点。** `mbd`、`slowpath_opt` handler 属于 `[vivo_slowpath_opt]`；`system_heap/charge` handler 属于 `[qcom_dma_heaps]`。
6. **【观察】指定目录没有 `vivo_rsc.ko`。** 对 `/vendor /system /odm` 的 `*rsc*`/`*csc*` 文件名搜索没有命中任何 `.ko`。当前符号和接口证据已覆盖本任务范围，故未额外拉 boot/vendor_boot；ioctl 数值 ABI 仍需二进制才能继续恢复。

## 1. 采集方法与模块身份

### 1.1 核心命令

```bash
export MSYS_NO_PATHCONV=1
ADB='D:/Android/SDK/platform-tools/adb.exe'
"$ADB" -s 10AFAB03FM003AB exec-out "su -c 'cat /proc/kallsyms'" \
  > analysis/device_pull/perf/vivo_rsc/kallsyms.root.txt
"$ADB" -s 10AFAB03FM003AB shell "su -c 'cat /proc/sys/kernel/kptr_restrict'"
"$ADB" -s 10AFAB03FM003AB shell "su -c 'grep vivo_rsc /proc/modules'"
"$ADB" -s 10AFAB03FM003AB shell "su -c 'ls -laR /proc/fas /proc/vapt /proc/vivo_rsc'"
```

### 1.2 身份证据

来源：`analysis/device_pull/perf/vivo_rsc/module-identity.txt:1-24`。

```text
Linux localhost 6.12.58-android16-6-gdd6fd5c98918-abogki543627742-4k ... aarch64
vivo_rsc 1368064 14 dwc3_msm,msm_kgsl,bwmon,shrinker_proxy,bpfhelper,memlat,
  qcom_lpm,csc_driver,symphony,zcache,vivo_slowpath_opt,swapmem_free,sched_walt,
  vivo_csc,[permanent], Live 0x0000000000000000 (O)
coresize=1368064
initsize=0
initstate=live
refcnt=14
scmversion=g41f446bcfc23-dirty
taint=O
parameters/enable_rsc=2 fastcgroup yes debug no
```

**【观察】** build-id note 的 20 字节 payload 为 `13 5e 30 8c fa 69 22 cd 59 6b f0 2d fc 5c 07 0a 2d 05 84 f0`。

**【推断】** 14 个 holder 表明它不是孤立调度策略，而是向 Qualcomm WALT/cpufreq/GPU/DDR、vivo CSC/slowpath 等模块提供内核内部接口。holder 列表证明静态模块依赖，不证明某个函数在本次采样时执行。

### 1.3 kallsyms 完整性及限制

- 原始完整文件：`analysis/device_pull/perf/vivo_rsc/kallsyms.root.txt`，507,878 行、24,055,034 字节。
- 模块子集：`analysis/device_pull/perf/vivo_rsc/kallsyms.vivo_rsc.txt`，7,791 行。
- SHA-256：完整文件 `cc1f721de135255c0ac825b8426062c2e928fa13bc0bcd9df2d1b2e83ca9fc04`；模块子集 `260430cb794881cd5f30dd2f5fe5d801c3dd6281b22be074a39c02ed944e8f19`，见 `analysis/device_pull/perf/vivo_rsc/SHA256SUMS.txt`。
- 命名符号类型：`t=1208`、`T=25`、`d=897`、`D=14`、`r=865`、`b=670`、`B=29`、`a=114`、`v=1`。
- `kptr_restrict=2` 和 `.text=0x0/.data=0x0` 的直接证据见 `module-identity.txt:9-20`。

## 2. 字面前缀统计

以下来自 `analysis/device_pull/perf/vivo_rsc/prefix-counts.tsv:1-25`。**统计允许重叠**，例如 `rsc_svp_` 同时计入 `rsc_`；“文本”仅指 kallsyms 类型 `t/T`，不代表运行时覆盖率。

| 前缀 | 命名符号 | t/T | 其他 |
|---|---:|---:|---:|
| `fas_` | 654 | 354 | 300 |
| `fla_` | 23 | 18 | 5 |
| `svp_` | 87 | 38 | 49 |
| `rsc_` | 697 | 174 | 523 |
| `qos_` | 34 | 31 | 3 |
| `ktop` | 0 | 0 | 0 |
| `launch` | 2 | 2 | 0 |
| `cpucap` | 5 | 0 | 5 |
| `mbd` | 0 | 0 | 0 |
| `vapt_` | 107 | 41 | 66 |
| `dep_` | 20 | 8 | 12 |
| `frame_` | 9 | 6 | 3 |
| `futex_` | 12 | 3 | 9 |
| `memqos_` | 0 | 0 | 0 |
| `vivo_fas_` | 3 | 2 | 1 |
| `rsc_svp_` | 43 | 6 | 37 |
| `rsc_fas_` | 5 | 2 | 3 |
| `rsc_cpucap_` | 23 | 8 | 15 |
| `rsc_cpu_usage_` | 14 | 5 | 9 |
| `rsc_qos_` | 24 | 4 | 20 |
| `rsc_game_` | 45 | 20 | 25 |
| `game_` | 2 | 2 | 0 |
| `binder_` | 14 | 11 | 3 |
| `sched_` | 10 | 7 | 3 |

重要解释：

- **ktop 并非不存在。** 其核心命名是 `rsc_cpu_usage_*`，并含 `ktop_mon_start/end` tracepoint；所以 `ktop` 字面前缀为 0。
- **cpucap 的核心函数带 `rsc_` 前缀。** 如 `rsc_cpucap_adjust_handler`；裸 `cpucap` 只命中 5 个数据符号。
- **launch 相关函数不只两个。** 除 `show_launch/store_launch` 外，还有 `show_launch_clone_boost/store_launch_clone_boost`、`fla_comm_launch_*`、`rsc_set_clone_boost`。
- **mbd/memqos 不属于 `[vivo_rsc]`。** 它们在 `[vivo_slowpath_opt]` 中有明确 handler，见第 6 节。

## 3. 子系统函数与职责归纳

完整的逐符号归属见：

- `analysis/device_pull/perf/vivo_rsc/SYMBOL_CLASSIFICATION.md`：19 个机械链接单元、全部 1,233 个 `t/T` 符号、前缀统计、全局文本符号。
- `analysis/device_pull/perf/vivo_rsc/symbols.classified.tsv`：3,823 行逐符号表，字段为原始模块行号、地址、类型、名称、单元、功能组、命中前缀。
- `analysis/device_pull/perf/vivo_rsc/compilation-units.txt`：未按功能拆分的原始链接单元函数目录。

下表的“职责”均为**【推断】**，依据是同行列出的**【观察】符号名、sysfs 节点和只读输出**；没有反汇编支撑算法细节。

| 子系统 | 关键观察（类/函数即内核符号） | 职责归纳 |
|---|---|---|
| FAS 核心 | `fas_sched_waking_tracer`、`fas_update_trace_event`、`fas_flp_calculate_thread_cap_v2`、`fas_flp_set_task_uclamp`、`fas_flp_gpu_pwrlevel_set`；`/sys/rsc/fas/{frame_info,boost_info,cluster_info,runtime}` | 跟踪 render/pair 帧、jank/jerk 和运行延迟，计算线程能力需求，并联动 CPU uclamp/频率与 GPU 状态。具体闭环公式未知。 |
| FAS dependency/FLA | `dep_sched_waking_tracer`、`fas_recover_dep_qos`、`rsc_traverse_dep_tree_core`、`fas_frame_sched_healthy_check`、`fla_freq_lock_*`、`frame_set_priority_ioctl` | 维护渲染/音频/滚动任务的唤醒依赖图，按帧调度健康状态实施依赖 boost 和频率 QoS。 |
| FAS memory | `fas_mem_work`、`fas_presentfence_cb`、`memlat_freq_stub`、`fas_mem_final_revote`、`vivo_fas_register_vote_thres`；`fas_mem/{ddr_freq,vote_type,vote_thres}` | 将帧/呈现 fence 与 DDR、memlat 投票关联。`fas_mem` 的 8 个直接节点与符号吻合。 |
| FAS GPU | `fas_gpu_fence_cb`、`fas_pm_qos_timeout_cb`、`fas_gpu_set_dev_constraints`、`fas_gpu_qos_update_value`、`fas_gpu_register_cb` | 通过 fence、PM QoS 与 GPU callback 获取/约束 GPU 状态；具体投票单位未知。 |
| SVP/sched boost | `__do_svp_balance`、`pick_first_svp_task`、`rsc_check_starving_task`、`rsc_enqueue_task_fair`、`rsc_replace_next_task_fair`、`trigger_svp_balance` | 为 SVP 标记任务维护队列，参与选核、抢占、迁移和饥饿恢复。SVP 缩写展开没有证据，不臆测。 |
| QoS | `sys_set_qos_level`、`rsc_register_qos_pid`、`enqueue_qos_util/dequeue_qos_util`、`qos_cpu_overutilized`、`rsc_qos_check_preempt_wakeup`；`/sys/rsc/qos` | 维护 per-task QoS 状态，把 QoS util 纳入 runqueue/CPU 带宽、抢占与迁移判断；还暴露 kswapd 特殊 QoS 控制。等级编码未知。 |
| ktop | `rsc_cpu_usage_enable/disable`、`do_cpu_usage_sort`、`account_task_time_hook`、`do_cpu_usage_notify`、`rsc_trace_ktop_start/end` | 周期统计并排序任务 CPU 时间，输出 top-task 快照和通知/trace。只读 `/sys/rsc/ktop` 实测含 per-CPU 使用率及按 usg 排序的线程表。 |
| launch/clone/app boost | `show_launch/store_launch`、`show_launch_clone_boost/store_launch_clone_boost`、`rsc_set_clone_boost`、`rsc_sched_boost_set`、`sched_boost_timeout` | 用户态写入启动/克隆场景后，触发带超时恢复的调度 boost；`appboost_late` 与 `sched_boost` 提供配套控制。 |
| CPUCAP | `rsc_cpucap_adjust_handler/internal`、`rsc_cpucap_enqueue`、`rsc_register_cpucap_adjust`、`cpu_frequency_limits_hook`、`policy_freq_adjust_notifier` | 跟踪 CPU possible/active/online、频率限制与 mitigated capacity，并向注册者发送调整通知。实测 `rsc_cpu_status` 显示 0-7 全部 possible/active/online。 |
| VAPT | `vapt_sched_waking_tracer`、`vapt_cpu_frequency_tracer`、`vapt_proc_info2_poll`、`vapt_procfs_create_pid_file_locked` | 提供另一套唤醒/频率跟踪与 per-pid procfs 状态、阈值、周期、poll 接口。VAPT 缩写和消息格式未知。 |
| 游戏调度 | `rsc_game_select_task_rq_rt/fair`、`rsc_game_find_lowest_rq`、`rsc_game_replace_next_task_fair`、`rsc_game_check_set_vip_cpu`；`svp/game_*` | 对游戏任务单独执行 RT/CFS 选核、低负载 runqueue 查找和 VIP CPU 判断。 |
| Binder/锁继承 | `binder_proc_transaction_hook`、`binder_thread_set_svp_inherit`、`mutex_svp_inherit_enqueue`、`android_vh_do_futex_handler`、`record_lock_starttime_*` | 将 SVP/QoS 属性沿 Binder、mutex、rwsem、percpu-rwsem、futex 等待链传播，并对锁等待超时做 promotion/统计。 |
| RSC core/cgroup | `do_rsc_gki_group_late_init`、`rsc_cgroup_init`、`check_rsc_cgroup`、`rsc_register_schceduler_vendor_hooks` | 建立共享 `/sys/rsc`、cgroup 映射和 vendor hooks。模块参数实测为 `enable_rsc=2 fastcgroup yes debug no`。 |

## 4. 用户态接口全景

### 4.1 sysfs：`/sys/rsc`

只读递归列表存于 `analysis/device_pull/perf/vivo_rsc/sys-rsc-ls-laR.txt`；规范化路径清单存于 `sys-rsc-files.txt`，共 **293 个文件**。直接子文件计数如下（不含继续嵌套的子目录）：

| 目录 | 直接文件数 | 归属证据 |
|---|---:|---|
| `/sys/rsc` | 35 | 多数 show/store 与 `[vivo_rsc]` 名称直接对应 |
| `/sys/rsc/fas` | 33 | `[vivo_rsc]` FAS/dependency handlers |
| `/sys/rsc/fas/fas_gpu` | 3 | `[vivo_rsc]` |
| `/sys/rsc/fas/fas_mem` | 8 | `[vivo_rsc]` |
| `/sys/rsc/fas/param` | 116 | `[vivo_rsc]` adaptive/jerk/migrate 参数 handlers |
| `/sys/rsc/qos` | 18 | `[vivo_rsc]` |
| `/sys/rsc/svp` | 50 | `[vivo_rsc]` SVP、游戏调度及混合策略 handlers |
| `/sys/rsc/mbd` | 5 | **`[vivo_slowpath_opt]`** |
| `/sys/rsc/slowpath_opt` | 7 | **`[vivo_slowpath_opt]`**，另有嵌套 memqos 目录 |
| `/sys/rsc/system_heap` | 1 | **`[qcom_dma_heaps]`**，另有 `charge` 子目录 |

计数证据：`analysis/device_pull/perf/vivo_rsc/sys-rsc-file-counts.txt:1-10`。

只读快照（来源：`sys-rsc-selected-values.txt`）：

```text
/sys/rsc/rsc_support = 1
/sys/rsc/fas/enable = 0
/sys/rsc/fas/frame_info:
total smooth normal jank jerk_1 jerk_2 ...
201   107    0      94   86     163 ...

/sys/rsc/fas/cluster_info:
cluster 0  cpu 0-5  max 762
cluster 1  cpu 6-7  max 1024
```

这些值只代表采样瞬间，尤其 `fas/enable=0` 不能解释为永久关闭。完整 `cluster_info` 还给出各簇频率/cap 表，证明 FAS 使用 capacity 域而不只是 kHz。

### 4.2 procfs 与 ioctl

路径和权限是**【观察】**，来源 `analysis/device_pull/perf/vivo_rsc/proc-interface-observations.txt:1-36`。ops/handler 名称是**【观察】**，来源 `kallsyms.root.txt:472977,473628-473641,473737-473812,473977-473981,474301,475193-475197`。二者按唯一名称对应是**【强匹配推断】**，未读取 ops 结构体指针。

| 当前路径与权限 | 对应 ops / handler | 读写观察与结论 |
|---|---|---|
| `/proc/fas/audio` `0664 root:root` | `fas_audio_ops`；`fas_audio_open/show/ioctl` | 空 read；存在 ioctl handler。 |
| `/proc/fas/scene` `0664 root:root` | `fas_scroll_ops`；`fas_scroll_open/show/ioctl` | 空 read；scene→scroll 为名称强匹配推断。 |
| `/proc/fas/monitor` `0664 root:root` | `fas_scroll_monitor_ops`；`..._open/show/ioctl` | 空 read；存在 ioctl handler。 |
| `/proc/fas/render` `0664 root:root` | `fas_render_ops`；`fas_render_open/show/ioctl` | 空 read；存在 ioctl handler。 |
| `/proc/vivo_rsc/frame_set_priority` `0660 root:root` | `frame_set_priority_fops`；`..._open/show/ioctl` | 空 read；存在 ioctl handler。 |
| `/proc/vivo_rsc/binder_boost_desc` `0664 system:system` | `binder_boost_desc_fops`；`binder_boost_show/store` | read 返回 30 个 desc 槽；首项 `id=0 code=-1 level=2`，次项 `id=1 code=10 level=2`，其余多为 -1。 |
| `/proc/vivo_rsc/show_set_task_qos` `0220 system:system` | `show_set_task_qos`、`show_set_task_qos_attr` | write-only proc 入口；未写入。 |
| `/proc/vapt/enable` `0664 system:system` | `vapt_proc_enable_ops`；open/show/write | read 为 `0`。 |
| `/proc/vapt/target` `0664 system:system` | `vapt_proc_target_ops`；open/show/write | 空 read。 |
| `/proc/vapt/render_record` `0664 system:system` | `vapt_proc_render_record_ops`；open/show | read 为表头 `pid tid pair`。 |

**【观察】** 模块内共有 5 个与当前 proc 路径对应的 `*_ioctl` handler，另有 `dep_render_ioctl_ext`、`vapt_render_ioctl_ext` 两个 `__ksymtab` 导出扩展函数。后者不是额外设备节点。

**【观察】** VAPT 还包含 `state/fps/info/threshold/period/info2` 的 proc ops 和 `vapt_procfs_create_pid_file_locked/remove_pid_file_locked`；当前目录未展示这些 per-pid 文件，符合动态创建能力。`info2` 是唯一名称明确的 poll handler。

**【未知】** 符号表不能给出 ioctl request 数值、`_IOC` 方向、参数结构体、兼容 32 位 ioctl 路径或每个命令的副作用；未执行任何 ioctl 探测。

### 4.3 `/dev` 与 `/proc/misc` 的有界否定结果

命令：

```bash
su -c 'cat /proc/misc'
su -c 'find /dev -maxdepth 3 \( -type c -o -type l \) 2>/dev/null |
       grep -Ei "rsc|fas|svp|qos|ktop|mbd|vivo|game|launch|cpucap"'
```

结果存于 `analysis/device_pull/perf/vivo_rsc/proc-misc-dev-candidates.txt:1-77`：

- `/proc/misc` 没有 rsc/fas/vapt/svp/qos/ktop/mbd 项。
- `/dev` 没有上述性能调度专用名称；仅匹配到 `fastrpc-*` 和通用 `vivo_haptic*`、`vivo_mem_scene`、`vivo_sync`、`vivo_fs_trace` 等其他接口，不能据此归属于 `vivo_rsc`。
- 因而已确认的 vivo_rsc 用户态 ABI 是 **sysfs + procfs**，不是独立 `/dev/vivo_rsc`。

### 4.4 内核内部接口

- `analysis/device_pull/perf/vivo_rsc/kernel-exports.txt` 从 `__ksymtab_*` 提取 **186 个导出项**，包含函数、变量和 tracepoint。
- `analysis/device_pull/perf/vivo_rsc/global-text-symbols.txt` 列出 **25 个类型 `T` 文本符号**。
- 代表性导出：`rsc_cpucap_adjust_register_notifier`、`qos_entity_before`、`rsc_consume_global_queue`、`vivo_fas_mem_register_fp_cb`、`fas_waltgov_freq_update`、`fas_gpu_register_cb`、`rsc_game_select_task_rq_fair`。
- 【观察】初始化符号包括 `rsc_register_schceduler_vendor_hooks`、`register_mutex_vendor_hooks`、`register_rwsem_vendor_hooks`、`register_percpu_rwsem_vendor_hooks`、`register_futex_vendor_hooks`。
- 【推断】性能策略不仅由用户态驱动，还通过 GKI vendor hooks 嵌入调度器、Binder 和锁慢路径；holder/exports/hook 三类证据相互印证。

## 5. `.ko` 搜索与 boot 决策

实际命令：

```bash
su -c 'find /vendor /system /odm -type f \
  \( -iname "*rsc*" -o -iname "*csc*" \) 2>/dev/null'
```

完整 35 个命中路径见 `analysis/device_pull/perf/vivo_rsc/filesystem-rsc-csc-search.txt:1-36`。命中的是 RenderScript、IMS rcconfig、传感器校准、配置及字体等，**没有任何 `.ko`，更没有 `vivo_rsc.ko`**，所以没有可 pull 的模块对象。

**【边界】** 此结果只覆盖当前挂载的 `/vendor`、`/system`、`/odm`；不排除对象曾位于 boot/vendor_boot ramdisk 或在加载后不再出现于这些文件系统。

**【决策】** 本轮不拉 boot 分区。任务是符号分类与接口定位，kallsyms、procfs、sysfs 和模块元数据已能回答；只有继续恢复真实汇编、ops 指针和 ioctl 数值 ABI 时，才有必要提取匹配 build-id 的 vmlinux/模块对象。

## 6. 容易误归属的共享节点

### 6.1 mbd / slowpath_opt

`[vivo_rsc]` 中没有 `mbd_*`、`memqos_*` 或 `refault_show`。相反，完整 kallsyms 明确给出：

```text
461010: b mbd_enabled                         [vivo_slowpath_opt]
461095: t count_workingset_refault            [vivo_slowpath_opt]
461118: t slowpath_show                       [vivo_slowpath_opt]
461121: t refault_show                        [vivo_slowpath_opt]
461130: t mbd_enable_show                     [vivo_slowpath_opt]
461133: t mbd_enable_store                    [vivo_slowpath_opt]
461297-461325: memqos_* handlers              [vivo_slowpath_opt]
```

因此 `/sys/rsc/mbd` 的 `refault/slowpath` 统计和 memqos 控制不能作为 vivo_rsc 函数统计。`vivo_slowpath_opt` 同时是 vivo_rsc holder，说明二者有依赖，但不改变符号属主。

### 6.2 system_heap

`charge_enable_show/store`、`charge_pause_show/store`、`sys_heap_pools_charge_show/store` 位于 `[qcom_dma_heaps]`（完整 kallsyms 约 492626-492651 行），不是 `[vivo_rsc]`。

### 6.3 `/proc/sys/walt`

`/proc/sys/walt/rsc_load_*` 等名称含 rsc，但其路径属于 WALT sysctl 树；不能仅凭字符串并入 vivo_rsc 接口。本报告只把 `/sys/rsc`、上述 proc 项和明确 `[vivo_rsc]` exports 纳入主清单。

## 7. 未解决问题（unknowns）

1. **真实加载载体**：当前证据支持“永久驻留 out-of-tree 模块”，但未确认来自 boot、vendor_boot 或其他早期 ramdisk；未证实静态链入 vmlinux。
2. **真实地址和汇编**：`kptr_restrict=2` 导致全 0；未为绕限制而修改设备。没有匹配二进制作反汇编。
3. **ioctl ABI**：5 个 handler 已定位，但 request 编码、参数布局、权限检查细节和 compat 行为未知。
4. **ops 指针级确认**：路径与 ops 的对应依赖唯一名称强匹配；尚未从二进制读取 `proc_ops/file_operations` 字段。
5. **动态 VAPT 文件**：per-pid state/fps/info/threshold/period/info2 的创建条件、写入协议和 poll 就绪语义未知。
6. **算法与缩写**：SVP/VAPT/MBD 的完整英文展开、QoS level 编码、FAS adaptive 公式、DDR/GPU 投票单位均无足够证据。
7. **共享根目录全覆盖归属**：293 个 `/sys/rsc` 文件已全部列出，但本方向只详细核对了 vivo_rsc、vivo_slowpath_opt、qcom_dma_heaps 的关键边界，没有逐文件反查所有其他属主。
8. **运行时覆盖率**：静态符号存在不证明当前执行；本任务未做 kprobe、eBPF、动态写节点或 ioctl 调用。

## 8. 交付文件索引

主报告：`analysis/device_pull/perf/REPORT_direction5_vivo_rsc.md`。

证据与清单位于 `analysis/device_pull/perf/vivo_rsc/`：

- `kallsyms.root.txt`：root 完整 kallsyms。
- `kallsyms.vivo_rsc.txt`：7,791 行模块全量子集。
- `symbols.vivo_rsc.named.txt`：3,823 行非 mapping 原始符号。
- `symbols.classified.tsv`：逐符号单元/前缀分类。
- `SYMBOL_CLASSIFICATION.md`：统计及全部文本函数目录。
- `prefix-counts.tsv`：字面前缀计数。
- `compilation-units.txt`：机械链接单元目录。
- `kernel-exports.txt`、`global-text-symbols.txt`：内核内部接口。
- `classify_symbols.py`：可复现分类脚本。
- `module-identity.txt`：内核、模块、build-id、kptr 证据。
- `filesystem-rsc-csc-search.txt`：`.ko` 搜索原始结果。
- `proc-misc-dev-candidates.txt`、`proc-interface-observations.txt`：misc/dev/proc 接口证据。
- `sys-rsc-ls-laR.txt`、`sys-rsc-files.txt`、`sys-rsc-file-counts.txt`、`sys-rsc-selected-values.txt`：sysfs 权限、路径、计数和只读快照。
- `SHA256SUMS.txt`：两份 kallsyms 的完整性摘要。
