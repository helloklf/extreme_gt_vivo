# 方向 7：系统属性 / init / SELinux / vendor 库 / framework 配置链路

## 1. 范围、目标与方法

- 设备：vivo V2505A，`ro.product.vendor.device=PD2505`，Android 17，SDK 37。
- adb serial：`10AFAB03FM003AB`；root：`su -c`，观察到 root context 为 `u:r:ksu:s0`。
- 全程只读：仅执行 `getprop`、`ls`、`ps`、`grep`、`find`、`strings`、`cat`、`dexdump`、`adb pull`；未执行 `setprop`、写节点、`chown`、`chmod` 或设备文件修改。
- 本机 `which jadx jadx.bat apktool` 均无结果；使用设备端 `strings`、本地 Python 解压 DEX、SDK `dexdump.exe` 做指令级静态分析。
- 原始证据目录：`analysis/device_pull/perf/direction7/`。
- 注意：采集后段设备从 adb 消失，最终 `adb devices -l` 为空（`final_adb_devices.txt`）。未完成的检索在“未知项”中明确列出。

## 2. 结论摘要

### 2.1 两条主要配置链路（直接观察与推断分别标注）

1. **应用 framework API → vivo 原生性能服务 → vendor VPerf AIDL → `/sys/rsc`**
   - `vivo-framework.jar` 的 `com.vivo.framework.vperf.VPerformance.lambda$connectPerfServiceLocked$1` 直接调用：
     `ServiceManager.getService("vendor.vivoperfservice")`，再通过 `IVivoPerfManager.Stub.asInterface` 建立 Binder 客户端。
   - `vendor.vivoperfservice` 的 init 定义为 `/system/bin/vivoperfservice`，运行用户为 `system`，实际 SELinux domain 为 `vivoperfservice`。
   - `/vendor/bin/hw/vendor.vivo.hardware.vperf-service-aidl` 的字符串直接包含 `/sys/rsc/svp/cpulimit`、`svp_ui_boost`、`launcher_boost_rt`、`/sys/rsc/qos/set_task_qos` 和 `persist.vendor.vperf_launch_thermal`。
   - 该 AIDL 服务 init 用户为 `root`，实际 domain 为 `hal_vperf_default`；文本 CIL 明确允许此 domain 读写 `sysfs_rsc`。
   - **限制**：`vivoperfservice → vendor AIDL` 这一跳的代理调用尚未用指令或运行时证据闭合，见未知项。

2. **system_server 的 `vivo-services.jar` → native `JniUtils.writeFile` → `/sys/rsc`**
   - `QoSUtils.setEnableQoS(Z)` 把布尔值转换为 `"1"`/`"0"`，调用 `JniUtils.writeFile("/sys/rsc/qos/qos_enable", value)`。
   - `AppStartPolicy.markUIThread(uid,pid,processName)` 构造 `"3000 " + uid + " " + pid`，写入 `/sys/rsc/launch`。
   - `NewQoSUtils.oldSetThreadABinder(tid,set)` 构造 `"%d %d 4"`，写入 `/sys/rsc/rsc_svp`。
   - `QoSUtils.enableRscDepList` 写 `/sys/rsc/fas/deplist_waker`；`writeFrameQosList` 写 `/sys/rsc/frame_qos_list`。
   - `RTUtils$ThreadRTRequest.realSetThreadRT` 写 `/sys/rsc/sched_boost` 和 `/sys/rsc/svp/svp_ui_boost`。
   - `JniUtils.writeFile(String,String)` 的 dexdump access 为 `PUBLIC STATIC NATIVE`，没有 Java code；native 实现库尚未定位。

### 2.2 配置链路图

```text
[镜像默认属性]
/vendor/build.prop
  ro.vendor.extension_library=libqti-perfd-client.so
  ro.vendor.perf-hal.ver=3.0
  ro.vendor.perf.scroll_opt=1
  vendor.perf.framepacing.enable=1
  vendor.perf.qfps.enable=1
        |
        +--> QXPerformance.ScrollOptimizer / Qualcomm perf 服务链
             （本方向未发现其直接写 /sys/rsc 的证据）

[应用进程]
  com.vivo.framework.vperf.VPerformance
        | ServiceManager.getService("vendor.vivoperfservice")  [直接观察]
        v
  /system/bin/vivoperfservice
  user=system, domain=vivoperfservice
        |
        | 代理到 vendor VPerf AIDL                     [推断，未闭合]
        v
  /vendor/bin/hw/vendor.vivo.hardware.vperf-service-aidl
  user=root, domain=hal_vperf_default
        | 字符串中包含节点；hal_vperf_default 有 sysfs_rsc 读写授权
        +--> /sys/rsc/svp/cpulimit
        +--> /sys/rsc/svp/svp_ui_boost
        +--> /sys/rsc/svp/launcher_boost_rt
        +--> /sys/rsc/qos/set_task_qos

[system_server 侧静态代码]
  /system/framework/vivo-services.jar
  QoS / AppStart / VBoost / RTUtils
        | JniUtils.writeFile(path, value)              [直接观察]
        v
  /sys/rsc/{launch,rsc_svp,sched_boost,frame_qos_list,...}

[其他 native 直接路径引用]
  libvivo.vas.qos.ctrl.so --> rsc_svp, qos/set_task_qos
  libaudioclient.so       --> qos/set_task_qos
  camera/vcf 相关库       --> qos/set_task_qos、rsc_svp、system_heap 等
```

## 3. 系统属性：当前值与默认来源

### 3.1 采集命令

```bash
adb -s 10AFAB03FM003AB shell "su -c 'getprop | grep -iE \"rsc|fas|svp|game|perf|sched|ux|vboost\"'"
```

完整输出：`direction7/properties.txt`（120 行）。

```bash
adb -s 10AFAB03FM003AB shell "su -c 'grep -nHiE \"rsc|vperf|vivoperf|persist.sys.perfdata|persist.sys.scxrsc|persist.vendor.vperf|sys.vivo.tip_switch_perfmode|vendor.perf.qfps|vendor.perf.framepacing|ro.vendor.extension_library|ro.vendor.perf\" /vendor/build.prop /vendor/default.prop /odm/etc/build.prop /system_ext/etc/build.prop /system/build.prop /product/build.prop 2>/dev/null'"
```

输出原文：`direction7/build_props.txt`。

### 3.2 关键属性表

| 属性 | 当前值 | 已确认的默认/来源 | 证据与判断 |
|---|---:|---|---|
| `ro.vendor.extension_library` | `libqti-perfd-client.so` | `/vendor/build.prop:341` 同值 | 镜像默认已确认；属于 Qualcomm perfd 链，不等于 `/sys/rsc` 写入链 |
| `ro.vendor.perf-hal.ver` | `3.0` | `/vendor/build.prop:342` 同值 | 镜像默认已确认 |
| `ro.vendor.perf.scroll_opt` | `1` | `/vendor/build.prop:343` 同值 | `QXPerformance.ScrollOptimizer.b()` 用 `SystemProperties.getBoolean(key,false)`；代码 fallback=false，但镜像默认覆盖为 true |
| `vendor.perf.framepacing.enable` | `1` | `/vendor/build.prop:344` 同值 | 镜像默认已确认 |
| `vendor.perf.qfps.enable` | `1` | `/vendor/build.prop:345` 同值 | 镜像默认已确认 |
| `persist.vendor.vperf_launch_thermal` | `true` | **默认写入来源未知** | 在 VPerf AIDL executable 中观察到该属性名字符串；未在已查 build.prop/init 命中 |
| `persist.vendor.vperf_sched_schedstats` | `1` | **默认写入来源未知** | 当前值见 properties.txt:37；已扫描 native/framework 未找到对应字符串 |
| `persist.sys.perfdata.scene_switch` | `2` | **代码 fallback=0；属性设置者未知** | `ScreenOnPolicy.<clinit>` 调用 `SystemProperties.getInt(key,0)`，结果 `>0` 时允许 screen big data 上报；它不是直接的 rsc 节点写入 |
| `persist.sys.scxrsc.flag` | `0` | **未知** | 当前值见 properties.txt:32；本次 native/framework 检索未找到引用，不能仅凭名字认定其控制 vivo_rsc |
| `persist.sys.perf.tracectl` | `true` | **未知** | 当前值见 properties.txt:30 |
| `sys.vivo.tip_switch_perfmode` | `1-2` | **未知** | 当前值见 properties.txt:111；已扫描 native/framework 未找到对应字符串 |
| `sys.init.perf_lsm_hooks` | `1` | **未知** | 当前值见 properties.txt:108；设备掉线前未完成全文本来源扫描 |

服务状态属性还直接显示：

```text
init.svc.perf2-hal-1-0=running
init.svc.vendor.perfservice=running
init.svc.vendor.vivoperfservice=running
init.svc.vivo-vperf-hal-1-0=running
init.svc.irsc_util=stopped
```

### 3.3 `persist.sys.perfdata.scene_switch` 的指令证据

文件：`direction7/vivo_services_selected_methods.txt:193-216`。

```text
ScreenOnPolicy.<clinit>:
  const-string v0, "persist.sys.perfdata.scene_switch"
  const/4 v1, #int 0
  invoke-static {v0,v1}, Landroid/os/SystemProperties;.getInt:(String;I)I
  ...
  sput-boolean ..., ScreenOnPolicy.isAllowReportScreenBigData:Z
```

结论：**当前值 2 是观察值；0 是该读取点的代码缺省值；不能据此推断属性由谁设为 2。**

## 4. init 配置、进程身份与节点权限

### 4.1 `/sys/rsc` 在指定 init 目录中的检索

命令：

```bash
adb -s 10AFAB03FM003AB shell "su -c 'grep -RIn -C 2 \"/sys/rsc\" /vendor/etc/init /system_ext/etc/init /system/etc/init /odm/etc/init 2>/dev/null'"
```

结果：**0 行**（`direction7/init_sys_rsc.txt`）。

因此，指定 init 目录中没有观察到这些节点的 `chown`/`chmod` 文本。节点权限可能由内核 attribute 注册直接指定，也可能由其他尚未覆盖的启动代码设置；本报告不臆测来源。

### 4.2 服务 rc 与实际进程

命令输出全文：`direction7/perf_service_rc.txt`、`direction7/process_contexts.txt`。

```text
/vendor/etc/init/vendor.vivo.hardware.vperf-service.rc:
service vivo-vperf-hal-1-0 /vendor/bin/hw/vendor.vivo.hardware.vperf-service-aidl
    class hal
    user root
    group root readproc oem_2907 system

/system_ext/etc/init/perfservice.rc:
service vendor.perfservice /system_ext/bin/perfservice
    class main
    user system
    group system readproc

/system/etc/init/vivoperfservice.rc:
service vendor.vivoperfservice /system/bin/vivoperfservice
    class main
    user system
    group system readproc
```

实际进程：

```text
u:r:hal_vperf_default:s0   root    1969  vendor.vivo.hardware.vperf-service-aidl
u:r:vivoperfservice:s0     system  2946  vivoperfservice
u:r:vendor_perfservice:s0  system  2961  perfservice
u:r:system_server:s0       system  3839  system_server
u:r:system_app:s0          system  5655  com.vivo.gamewatch
```

### 4.3 当前节点 owner/mode/context

命令：

```bash
adb -s 10AFAB03FM003AB shell "su -c 'ls -lZR /sys/rsc'"
```

完整输出：`direction7/sys_rsc_ls_lZR.txt`（355 行）。本地解析结果：

- 共 308 个对象：293 个文件、15 个目录。
- 286 个文件为 `system:system`，7 个文件为 `root:root`，目录均为 `root:root`。
- 文件 mode 分布：`0660` 91 个、`0664` 151 个、`0640` 3 个、`0444` 9 个、`0440` 17 个、`0220` 1 个、`0644` 20 个、`0200` 1 个。

关键片段：

```text
-rw-rw---- system system sysfs_rsc  /sys/rsc/rsc_svp
-rw-rw---- system system sysfs_rsc  /sys/rsc/rsc_switch
-rw-rw---- system system sysfs_rsc  /sys/rsc/sched_boost
-rw-rw-r-- system system sysfs_rsc  /sys/rsc/fas/enable
-rw-rw-r-- system system sysfs_rsc  /sys/rsc/fas/target
-rw-rw---- system system sysfs_rsc  /sys/rsc/qos/set_task_qos
-rw-rw---- system system sysfs_rsc  /sys/rsc/svp/svp_enable
-rw-rw---- system system sysfs_rsc  /sys/rsc/svp/game_mode
-rw-rw---- system system sysfs_rsc  /sys/rsc/frame_qos_list  (proc_frame_qos_list)
-rw-rw-r-- system system sysfs_rsc  /sys/rsc/frame_set_priority_enable (proc_frame_set_priority_enable)
```

解释：

- 对大多数 `0660/0664 system:system` 节点，**UNIX UID 直接指向 system（UID 1000）可写**，与 system_server、system_app、vivoperfservice 等身份一致。
- VPerf AIDL 是 root，不能只用 owner 排除它；其 SELinux domain 也有显式写授权。
- 7 个 root 文件例外为：`debug`、`enabled`、`func_debug`、`fas/dep_init_count`、`fas/deplist_wakee`、`slowpath_opt/folio_lock_owner_enable`、`slowpath_opt/folio_lock_owner_xa`。

## 5. SELinux 证据

### 5.1 类型与 genfscon

命令：遍历 `/vendor/etc/selinux`、`/system/etc/selinux`、`/system_ext/etc/selinux`、`/odm/etc/selinux` 下的 `*.cil`/`*.conf` 并 grep `rsc|fas|svp|vboost|sys/rsc`。

完整命中：`direction7/sepolicy_rsc.txt`。关键原文来自 `/vendor/etc/selinux/vendor_sepolicy.cil`：

```text
603:  (genfscon sysfs "/rsc/frame_set_priority_enable" (... proc_frame_set_priority_enable ...))
683:  (genfscon sysfs "/rsc/frame_qos_list" (... proc_frame_qos_list ...))
772:  (genfscon sysfs "/rsc" (... sysfs_rsc ...))
3990: (type sysfs_rsc)
3992: (type proc_rsc)
4716: (type proc_fas)
```

`ls -Z` 的实际节点标签与 genfscon 一致：绝大多数为 `u:object_r:sysfs_rsc:s0`，两个特殊节点分别为 `proc_frame_qos_list`、`proc_frame_set_priority_enable`。

### 5.2 直接读写 `sysfs_rsc` 的授权域

文本 CIL 中观察到：

- `hal_vperf_default`：file/dir 的 read/write/open/search 等完整权限（CIL 15353-15354）。
- `system_server_202504`：file 的 read/write 等（16585）。
- `system_app_202504`：file/dir 的 read/write 等（16360、16363）。
- `vivo_daemon_202504`：file 的 read/write 等（17277-17278）。
- `hal_camera3rd_default`、`hal_vibrator_default`、`hal_camera_default`：各自 file 写权限。
- `init_202504`：setattr/read/write/open 等（15442、15482-15483）。
- `atrace_202504`、`dumpstate_202504`、`surfaceflinger_202504`、`vivonrtservice_202504`：特定 file write/open 权限。
- `platform_app_202504`、`lmkd_202504`、`vivocameraserver_202504` 等仅观察到读或目录权限，不应归为通用写者。

**有边界的负结果**：同一文本 CIL 检索未见 `vivoperfservice` 或 `vendor_perfservice` 直接对 `sysfs_rsc` 的 allow，也未见两个 `proc_frame_*` 特殊类型的显式 allow。该结果只覆盖已扫描文本 CIL，不能代替完整编译策略查询。

**运行时限制**：设备掉线后 `getenforce` 未成功，故本报告只陈述策略规则与进程 label，不声称当前 SELinux enforcement 状态。

## 6. native 库与可执行文件

### 6.1 检索范围与方法

- 枚举 `/vendor/lib64`、`/system_ext/lib64` 的 perf 候选库，结果见 `direction7/native_candidates.txt`（36 项）。
- 对两个目录的全部一级 `.so` 以及三个服务 executable 执行 `strings -a`，检索 `/sys/rsc` 和关键属性。
- 另补扫 `/system/lib64` 一级 `.so`，结果见 `direction7/system_native_rsc_strings.txt`。

### 6.2 直接命中

| 文件 | 直接观察到的字符串 |
|---|---|
| `/vendor/bin/hw/vendor.vivo.hardware.vperf-service-aidl` | `/sys/rsc/svp/cpulimit`、`svp_ui_boost`、`launcher_boost_rt`、`/sys/rsc/qos/set_task_qos`、`persist.vendor.vperf_launch_thermal` |
| `/vendor/lib64/libvivo.vas.qos.ctrl.so` | `/sys/rsc/rsc_svp`、`/sys/rsc/qos/set_task_qos` |
| `/vendor/lib64/libvivo.vaf.system.so` | `/sys/rsc/qos/set_task_qos` |
| `/system/lib64/libaudioclient.so` | `/sys/rsc/qos/set_task_qos` |
| `/system/lib64/libVivoDmService.so` | 包含只读采集命令字符串：`cat /sys/rsc/ktop >> /data/bbklog/systemserver/wdog_ktop.txt`（本次未执行该命令） |
| `libcamxcommonutils.so`、`libchifeature2.so`、`libvcf_camera_aidl_device.so`、`libvcf_features.so`、`libvcf_session.so`、`libvcf_utils.so` | 均为 `/sys/rsc/qos/set_task_qos` |
| `/vendor/lib64/libvcf_platform_utils.so` | `system_heap/charge/sys_heap_pools_charge`、`rsc_svp`、`qos/set_task_qos`、`svp/kswapd_affinity_limit`、`system_heap/memory_info`、`qos/set_kswapd_qos_level` |
| `/vendor/lib64/libvivo_raw_nr_cpu.so` | `/sys/rsc/system_heap/memory_info` |

原始输出：`direction7/native_rsc_strings.txt`、`direction7/system_native_rsc_strings.txt`。

这些字符串证明二进制携带路径，**不单独证明当前运行时确实写入了节点**；方法归属或运行时跟踪需要另做。

### 6.3 有边界的负结果

- `/system_ext/lib64` 一级 `.so` 未命中 `/sys/rsc`。
- 请求的候选模式中未发现 `libfrm*`、`*fpsgo*`、`libsched*`；存在 `libRSCpuRef.so`，但名字中的 RS 是 RenderScript CPU 路径，不能当作 vivo_rsc。
- `/system/bin/vivoperfservice`、`/system_ext/bin/perfservice` 的本轮字符串筛选未命中 `/sys/rsc`。这与“可能通过 vendor AIDL 间接访问”相容，但不是代理链的充分证明。

## 7. framework jar / DEX 证据

### 7.1 工件身份与分析方式

拉取：

- `/system/framework/vivo-framework.jar`，SHA-256 `72e09685b07d899e22fb8c2449a2fd413b4bd5d58277a979d7215865ea340053`
- `/system/framework/vivo-services.jar`，SHA-256 `304411a948919e51985d601e819d1d2b1a2ec819c13d2f0fd6e0f784ce33a5df`
- `QPerformance.jar`、`QXPerformance.jar`、`UxPerformance.jar`、`boot-vivo-framework.vdex`

REA 对 `vivo-services.jar` 的 ZIP 中央目录完整盘点：4 个 occurrence、0 个 integrity contradiction；Evidence `ev_45fdd62990cc8ece0f600f24427d5a1702d3eca5ee59b958aff811c66fc39ac6`。REA 盘点不证明运行行为，代码结论来自后续 DEX 指令。

Python 先解压 DEX 再提取字符串，避免压缩 jar 的 strings 漏报。输出：`direction7/framework_dex_strings.txt`。随后用 `D:/Android/SDK/build-tools/36.0.0/dexdump.exe -d` 定位到方法。

### 7.2 应用 API 到 `vendor.vivoperfservice`

`VPerformance.lambda$connectPerfServiceLocked$1` 的关键指令（`direction7/vperformance_selected_methods.txt`）：

```text
const-string/jumbo v0, "vendor.vivoperfservice"
invoke-static {v0}, Landroid/os/ServiceManager;.getService:(String;)Landroid/os/IBinder;
...
invoke-static {v0}, Lcom/vivo/framework/vperf/IVivoPerfManager$Stub;.asInterface:(IBinder;)...
sput-object ..., VPerformance.sPerfService:IVivoPerfManager
...
invoke-interface ..., IVivoPerfManager.setClientBinder:(IBinder;)I
```

结论：**VPerformance 是 Binder 客户端，不是直接 `/sys/rsc` 写入者。** `vivo-framework.jar` 中另见 `/proc/fas/monitor`（`com.vivo.vspa.loadaware.config.PolicyConfig.<clinit>`），但未发现 `/sys/rsc` 字符串。

### 7.3 system_server 侧直接节点操作

原始方法引用：`direction7/vivo_services_classes2_method_refs.txt`；选定方法完整指令：`direction7/vivo_services_selected_methods.txt`。

| 类.方法 | 指令级逻辑摘要 | 节点 |
|---|---|---|
| `QoSUtils.setEnableQoS(Z)` | boolean 转 `"1"`/`"0"`，调用 native writeFile | `/sys/rsc/qos/qos_enable` |
| `AppStartPolicy.markUIThread(I,I,String)` | 构造 `"3000 "+uid+" "+pid`，调用 native writeFile | `/sys/rsc/launch` |
| `NewQoSUtils.oldSetThreadABinder(IZ)` | 以 tid 和 set?1:0 格式化 `"%d %d 4"`，调用 native writeFile | `/sys/rsc/rsc_svp` |
| `QoSUtils.enableRscDepList` | 构造参数后调用 native writeFile | `/sys/rsc/fas/deplist_waker` |
| `QoSUtils.writeFrameQosList` | 两个重载均调用 native writeFile | `/sys/rsc/frame_qos_list` |
| `RTUtils$ThreadRTRequest.realSetThreadRT` | 多个分支调用 native writeFile | `/sys/rsc/sched_boost`、`/sys/rsc/svp/svp_ui_boost` |
| `RSCSupportUtils.rscVersion` | `FileUtils.readKernelNode(path)` | 读 `/sys/rsc/rsc_support` |
| `vboost.utils.DeviceUtils.kernelSupportVersion` | `VBoostFileUtils.readLine(File)` | 读 `/sys/rsc/svp/support_vboostsdk` |

`JniUtils.writeFile` 的 dexdump（`direction7/jni_utils_methods.txt`）：

```text
name: 'writeFile'
type: '(Ljava/lang/String;Ljava/lang/String;)I'
access: 0x0109 (PUBLIC STATIC NATIVE)
code: (none)
```

### 7.4 Qualcomm 属性读取点

`QXPerformance.ScrollOptimizer.b()`（`direction7/qxperformance_scrolloptimizer_b.txt`）：

```text
const-string v3, "ro.vendor.perf.scroll_opt"
const/4 v2, #int 0
invoke-static {v3,v2}, Landroid/os/SystemProperties;.getBoolean:(String;Z)Z
sput-boolean ..., ScrollOptimizer.x:Z
```

这给出“代码缺省 false + `/vendor/build.prop` 默认 1 + 当前值 1”的闭环。

### 7.5 静态引用与当前内核节点的版本差

`vivo-services.jar` 还引用下列当前 `ls -lZR /sys/rsc` 未观察到的路径：

```text
/sys/rsc/fas/fla_echo_break_freq_enable
/sys/rsc/fas/resume_cpus
/sys/rsc/fas/fla_boost_uclamp
/sys/rsc/rsc_gpu_freq_break
/sys/rsc/qos/qos_ni_enable
/sys/rsc/qos/qos_stat
/sys/rsc/svp/support_vboostsdk
/sys/rsc/binder_cmd
/sys/rsc/binder_event
/sys/rsc/binder_frozen_callback
```

这些应视为**条件路径、旧版/新版路径或当前配置未启用路径**，不能报告为当前可写节点。静态代码存在也不证明对应分支在当前设备执行。

## 8. 未知项与后续建议

1. **设备掉线导致未闭环**：`vivoperfservice` 到 vendor VPerf AIDL 的代理调用尚未用 native 指令或 Binder 运行时证据确认；当前仅有服务名、proxy 库名、AIDL 字符串和节点权限的间接证据。
2. **`JniUtils.writeFile` native 实现库未知**：已确认 Java native 声明，但未定位具体 `.so` 和函数符号；设备恢复后可对 `/system/lib64/libandroid_servers.so` 及 vivo 相关库搜 `JniUtils`/`RegisterNatives`。
3. **属性设置者与 fallback 不全**：`persist.vendor.vperf_launch_thermal` 只确认 AIDL binary 含属性名；`persist.vendor.vperf_sched_schedstats`、`persist.sys.scxrsc.flag`、`sys.vivo.tip_switch_perfmode` 等来源仍未知。设备掉线使全文本来源补扫未完成。
4. **SELinux 当前模式未知**：最终 `getenforce` 未成功；已完成的是实际 label、进程 domain 和文本 CIL 授权分析。
5. **init 权限来源未知**：四个指定 init 目录没有 `/sys/rsc` 命中，不能确定 owner/mode 是内核注册时设置还是其他启动代码设置。
6. **静态路径不等于运行时写入**：本报告没有动态追踪 open/write，也没有修改节点做验证；尤其是当前不存在的兼容路径，必须保留为未知。
7. **工具边界**：无 jadx/apktool，因此引用的是 dexdump 指令和方法级逻辑摘要，而非完整 Java 反编译源码。
