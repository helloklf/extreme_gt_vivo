# 方向 2：GameCube.apk 静态逆向报告

## 0. 范围、样本与方法

目标仅为 `com.vivo.gamecube` 的结构、游戏识别、性能模式、跨组件调用和侧边栏请求链。未修改设备节点、属性或文件，未点击 UI，也未执行目标应用。

【观察】本机样本与设备样本完全相同：

```text
$ sha256sum analysis/device_pull/perf/GameCube.apk
2fe0c05983991b1b829918ccbc2f49658409fab4a259ec4330bdd894408370ba  GameCube.apk

$ adb -s 10AFAB03FM003AB shell "su -c 'sha256sum /system/app/GameCube/GameCube.apk'"
2fe0c05983991b1b829918ccbc2f49658409fab4a259ec4330bdd894408370ba  /system/app/GameCube/GameCube.apk
```

APK 为 85,071,913 字节，包版本 `15.0.06.02_de` / `1500602`，`minSdk=29`、`targetSdk=36`。Manifest 声明 `sharedUserId="android.uid.system"` 和 `coreApp=true`；安装路径为 `/system/app/GameCube/GameCube.apk`。

工具边界：

- `which jadx jadx.bat apktool` 均无结果；REA `inspect_android_package` 返回 `unsupported_provider`：`Android JADX subprocess ownership is unsupported on win32/arm64`。
- 找到 `C:/Users/hello/apktool/apktool.jar`，但 Java 在本机连续报告内存提交失败，未生成可用 Smali 全量结果；失败日志已删除。
- 改用 SDK `aapt2` 解 Manifest/资源，以及设备自带 `/apex/com.android.art/bin/dexdump` 直接读取 APK。`dexdump` 只读设备文件，输出仅写本机：

```bash
export MSYS_NO_PATHCONV=1
D:/Android/SDK/platform-tools/adb.exe -s 10AFAB03FM003AB \
  exec-out "su -c '/apex/com.android.art/bin/dexdump -d -n /system/app/GameCube/GameCube.apk'" \
  > analysis/device_pull/perf/gamecube_static/GameCube.dexdump.txt
```

结果为 397,671,082 字节、24,661 个 `Class #` 声明，覆盖四个 DEX 的类/方法/指令。后续“方法逻辑摘要”均由 DEX 指令归约，不当作原始 Java 源码。混淆类名保留，语义名仅在日志或接口明确时使用。

主要证据索引：

- `gamecube_static/AndroidManifest.txt`：aapt2 解出的 Manifest。
- `gamecube_static/manifest_components.txt`：93 个组件的 compact 清单。
- `gamecube_static/resources.txt`：资源名和多语言值。
- `gamecube_static/assets/`：APK 自带配置。
- `gamecube_static/GameCube.dexdump.txt`：完整反汇编。
- `gamecube_static/focused/`、`focused2/`：按类提取的关键证据，便于复核。

## 1. APK 解包结构

### 1.1 ZIP/DEX/原生库

【观察】ZIP 共 6,002 项：`res` 5,745、`assets` 148 个文件、4 个 DEX。DEX 大小分别为：

```text
classes.dex   8,516,600
classes2.dex  8,854,976
classes3.dex  5,953,616
classes4.dex  2,890,780
```

对 ZIP 全路径检查结果为：

```text
lib entries []
so anywhere []
native shader files 48
lottie/json 87
```

因此 **APK 内没有 `lib/arm64-v8a/`，也没有任何 ABI 的 `.so`**，没有可对其执行 `elfscan.py` 的打包原生库。48 个 GLSL 文件是文本 shader，不是 ELF。

【观察】虽然没有打包 `.so`，仍有原生依赖声明和 JNI 使用：

- Manifest `AndroidManifest.txt:1380-1397` 声明可选 `libOpenCL.so`、`libvcap_npu_network.so`、`libvcap_npu_network_v1.so`，均 `required=false`。
- `com.qti.acg.gpuprofile_manager.ProfileManager.<clinit>` 调 `System.loadLibrary("profilemanager-jni")`；同类的 `nativeSaveProfile(String)`、`nativeSyncProfiles(String)` 是 `PRIVATE STATIC NATIVE`，见 `focused2/ProfileManager.txt:23-25,218-227`。
- 【未知】该 JNI 库由系统/ vendor 提供还是随其他组件提供，超出本 APK 内容；本报告未对该外部库作行为断言。

### 1.2 Manifest 组件

【观察】共 60 activity、15 service、6 receiver、12 provider。完整 compact 清单见 `gamecube_static/manifest_components.txt`。与性能/调用链最相关者：

| 组件 | exported / process | 作用与证据 |
|---|---|---|
| `GameCubeMainActivity` | true | 响应 `com.vivo.gamecube.MAIN/GAMECUBE` 等，见 Manifest:282-299 |
| `GameCubeSettings$CompetitionModeActivity` | true | 响应 `com.vivo.gamecube.COMPETITIONMODE`，见 Manifest:353-361 |
| `com.vivo.gamedaemon.GameDaemonService` | true / `:daemon` | `com.gamecube.daemon.action.GameDaemonService`，权限 `VAS_UNIFIED_MONITOR`，Manifest:1231-1247 |
| `com.vivo.gameassistant.AssistantUIService` | false / `:assistantui` | 侧边栏 UI 服务，Manifest:1345-1354 |
| `com.vivo.gameassistant.GameCubeFuncService` | true / `:assistantui` | 签名权限 `GAME_CUBE_FUNC_SERVICE`，Manifest:1355-1365 |
| `GameSettingsProvider` | true / `:daemon` | `content://com.vivo.gamecube.gamesettings.provider`，Manifest:1188-1194 |
| `DaemonProvider` | true / `:daemon` | `content://com.vivo.gamecube.provider`，权限 `GAME_PROVIDER`，Manifest:1195-1201 |
| `SpProvider` | true / `:daemon` | `com.vivo.gamecube.gamespprovider`，Manifest:1202-1208 |
| `CommunicationProvider` | false / `:assistantui` | `com.vivo.gamecube.communicationprovider`，但共享 system UID 下承担内部命令处理，Manifest:1296-1301 |

Manifest 还声明 `QUERY_ALL_PACKAGES`、`WRITE_SECURE_SETTINGS`、`SYSTEM_ALERT_WINDOW`、`com.iqoo.powersaving.permission.MODE_CHANGE` 和 `com.vivo.gamekit.permission.REQUEST_GAMETOOLS`。这些声明证明能力边界，不等于运行时一定使用。

### 1.3  Assets 中的名单/配置

【观察】关键文件：

- `GameCube.xml`：国内游戏主名单，版本 50，5,131 个 `<package>`，5,124 个不同包名。
- `GameCubeOversea.xml`：海外名单，5,018 个不同包名。
- `FunctionConfigList.xml`：版本 214，49 个功能名单，2,777 个 `<item>`；包含 `competition_mode`（27 项）、`game_watch_hook`（5 项）、`frame_optimization`（2 项）、`boost_frame`、`optimize_power` 等。
- `FunctionParams.xml`：版本 10，71 个包条目，描述电竞、变声、GameWatch hook 等渠道参数。
- `game_speed_apps.xml`：仅迅游/UU 两个加速器包名，不是主游戏名单。
- `profiles/` 下 5 个二进制 GPU profile：王者荣耀、和平精英、QQ 飞车、斗罗大陆魂师对决、BrightRidge。内容以 `type.googleapis.com/GraphicsProfilePrivate` 开头，是不透明二进制数据，不能据文件名臆测具体频率/画质值。
- `PD2301/FrameInterConfigList.xml`、大量触控/灯效/动画 JSON 和 48 个 GLSL 文本。

`FunctionConfigList.xml:163-195` 的 `competition_mode` 是功能白名单；它决定电竞功能是否可用，**不是**“判某 app 是否为游戏”的充分条件。

## 2. 游戏检测机制

### 2.1 主名单来源和更新

【观察】`com.vivo.gamedaemon.a0.B(String)` 按内外销选择 `GameCubeOversea.xml` 或 `GameCube.xml`，同时读取统一配置，比较版本并返回较高者：

```text
focused/com_vivo_gamedaemon_a0.txt:70-107
  k0()Z -> oversea ? GameCubeOversea.xml : GameCube.xml
  local = gamedaemon.utils.e.e(context, file)
  remote = gamedaemon.utils.e.h(inputFromUnifiedConfig)
  return remote.version > local.version ? remote : local
```

`a0.P(List)` 将解析后的名单清空并写入 `gameWhiteListTable(package_name,label,game_channel,data_1)`，见同文件 `:537-591`。因此本 APK 内置名单是基线，统一配置可覆盖更新。

### 2.2 已安装游戏如何被认可

【观察】调用链显示是 **白名单数据库 + 已安装包校验**，而不是仅按 Android `CATEGORY_GAME`：

1. `ea/b$n` 生成 `content://com.vivo.gamecube.provider/support_game`，见完整反汇编行 3615385-3615416。
2. `com.vivo.common.utils.m.Z2` 查询该 URI 的 `package_name` 并去重，见 `method_evidence.txt:2-95`。
3. `com.vivo.common.utils.m.e0` 在加入结果前调用 `m.B(context,pkg)` 获取 `ApplicationInfo`，包不存在则不加入，见 `method_evidence.txt:98-190`；新实现 `lj.a.f` 用 `m.t1(context,pkg)` 做同类检查，见 `:235-327`。
4. `com.vivo.gamedaemon.m0.C()` 在内存缓存为空时回退查询上述 provider，日志为 `getAddedList: memory cache empty, fallback to database`，见 `focused2/com_vivo_gamedaemon_m0.txt:965-1003`。
5. 对“添加所有应用”路径，`m0.R(String)` 会过滤系统 app 和一组硬编码黑名单，见同文件 `:317-367`。

全量 DEX 对 `CATEGORY_GAME|android.content.pm.ApplicationInfo.CATEGORY_GAME` 搜索为 0 命中。`QUERY_ALL_PACKAGES` 只说明可见性权限，不能据此推断采用系统游戏分类。

【推断】GameCube 的“游戏”主要是厂商名单/用户加入列表概念；`FunctionConfigList` 再对每个功能做第二轮硬件、渠道、包名黑白名单过滤。未观察到一个仅依赖 APK 类别或 native 特征的通用自动识别算法。

【未知】当前设备统一配置的实际版本、服务器是否另有在线识别规则，未联网查询；本结论针对随包基线和静态更新逻辑。

## 3. 性能模式与下发参数

这里必须区分三条路径：新版性能面板、旧 `competition_mode` 功能集合、旧自定义 GPU Profile。它们不是同一组参数。

### 3.1 新版性能面板：模式值与电源服务 command

【观察】`ModeType.<clinit>` 的枚举值见 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_modeshift_ModeType.txt:72-153`：

| UI/枚举 | `mValue` | `ModeShiftHelper.w()` 发给电源服务的 `command` |
|---|---:|---:|
| `POWER_SAVING`（省电） | 2 | **1005** |
| `NORMAL`（均衡） | 1 | **1004** |
| `MONSTER`（Monster/性能） | 5 | **1011** |
| `ENHANCED`（增强/BOOST） | 6 | **1014** |
| `MONSTER_PLUS` | 7 | 默认 **1014**；条件分支可为 1011 |
| `BOOST_PLUS` | 8 | 默认 **1014**；条件分支可为 1011 |
| `UNKNOWN` | -1 | 不用于正常下发 |

`ModeType.fromValue()` 未命中时回退 `NORMAL`。

`ModeShiftHelper.w(ModeType)` 在 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_modeshift_o.txt:752-857` 构造：

```text
Intent("intent.action.POWER_MODE_CHANGE_SERVICE")
  .setPackage("com.iqoo.powersaving")
  .putExtra("command", 上述整数)
  .putExtra("source", 23)
context.startService(intent)
```

DEX packed-switch 原始表也已直接解码，避免误配分支：

```text
1 NORMAL        -> 003e -> command=1004
2 POWER_SAVING  -> 0078 -> command=1005
3 MONSTER       -> 0076 -> command=1011
4 ENHANCED      -> 007a -> 初值 command=1014
5 MONSTER_PLUS  -> 0040 -> 默认 1014，特性条件可改为 1011
6 BOOST_PLUS    -> 0040 -> 同上
```

`game_plus_mode_key` 是附加状态，不应误当第五种 command：离开 Plus 时会写 0；进入 Plus 的特定背夹连接分支写 1，见 `modeshift_o.v(Z)` 和 `w()`。

【推断】GameCube 在新版面板中只下发“模式号 + source”，并不直接组包 CPU/GPU 频率、调度 mask 或 `/sys/rsc` 值。1004/1005/1011/1014 如何映射到 fas/svp/qos/ktop/mbd，是 `com.iqoo.powersaving` 及其下游的职责。

【未知】电源服务对四个 command 的最终内核参数和背夹 Plus 的完整差异，超出本 APK，不能从 UI 文案反推。

### 3.2 “自定义”到底是哪一种

新版 `ModeType` 没有 `CUSTOM` 枚举。`modeshift.o.g(pkg)` 使用 `key_mode_in_game_<pkg>` 保存每个游戏的上述 `mValue`，见 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_modeshift_o.txt:223-257`；`s(pkg,mode)` 写回同键，见 `:602-647`。因此按游戏记住不同档位是“按游戏配置”，不是另一种独立调度档位。

另有一个旧的 GPU mode 存储 `game_cube_gpu_mode`：`m.Z()` 将其按冒号拆成 `<pkg><末位模式>`，`m.Y()` 返回末位整数，见 `custom_mode_methods.txt:2-25` 和 `focused2/com_vivo_common_utils_m.txt` 的 `Z()`。这里 **3 才是 custom**，与新版 `mValue` 不可混用。

### 3.3 旧自定义 GPU Profile 的实际参数

【观察】`CommunicationProvider.call` 识别 `resetControlPanelMode` 并进入 `A()`，对王者荣耀、和平精英、QQ 飞车、斗罗大陆魂师对决、BrightRidge 五包处理，见 `focused2/focused_com_vivo_gameassistant_CommunicationProvider.txt:31-79` 和 `:3759-3764`。

`CommunicationProvider.j()` 的分支：

- mode 0：走 balance 分支，关闭 autoVRS。
- mode 1、2：共用 power-save/high-quality 分支，不能仅凭此函数进一步分别命名。
- mode 3：custom，读取已有 profile，并取出 `MaxSamples`、`TextureMaxAniso`、`TextureFilteringQuality`、`MipLodBias` 回写；缺 profile 时记录失败。
- 各分支最后都经过 `fa.b -> fa.e`（高通平台实现）保存/同步。

高通 `fa.e.e()` 明确构造两组键（`focused2/focused_fa_e.txt:97-157`）：

```text
GLES:   api=GLES, MaxSamples, TextureMaxAniso, MipLodBias,
        TextureFilteringQuality, DisablePrivateProfileData
Vulkan: api=Vulkan,
        ro.vendor.qcom.adreno.qgl.MaxSamples,
        ro.vendor.qcom.adreno.qgl.MaxTextureAnisotropy,
        ro.vendor.qcom.adreno.qgl.MipLodBias,
        ro.vendor.qcom.adreno.qgl.TextureFilteringQuality,
        ro.vendor.qcom.adreno.qgl.DisablePrivateProfileData
```

随后调用 `ProfileManager.c()` 和 `d()`，最终到 JNI `nativeSaveProfile/nativeSyncProfiles`。mode 3 的四个图形字段来自已有 profile；mode 0/1/2 的四个图形字段为空字符串。各分支的 `DisablePrivateProfileData` 均为 `"TRUE"`。这里观察到的是图形 Profile 参数，**不是 CPU/GPU 频率数值**。

### 3.4 旧 `competition_mode` 不是同一组调度参数

【观察】`electronic/b$b.subscribe()` 设置：

```text
gamecube_competition_mode_state = 0/1
gamecube_background_call_state = background_call_on
gamecube_block_notification_state = gamecube_block_notification_on
gamecube_lock_screen_brightness_state = lock_screen_bright_on
gamecube_shield_smart_key_state = disable_smart_key_on
gamecube_shield_screen_capture_state = disable_screen_capture_on
（按能力）缩短导航条/状态栏触发区
```

证据为 `focused2/com_vivo_gameassistant_electronic_b_b.txt:44-96`。`electronic/a.d()` 还会先检查 `ConfiguredFunction.COMPETITION_MODE` 白名单。

【推断】旧电竞模式是一组免打扰/通话/亮度/导航功能的组合开关，不能与 MONSTER 的 1011 直接等同。未在该路径看到 CPU/GPU 调度参数。

### 3.5 与 `/sys/rsc` 的关系

对完整反汇编分别搜索：

```text
/sys/rsc|vivo_rsc               -> 0 命中
\b(svp|qos|ktop|mbd)\b          -> 0 命中
```

所以本 APK 内未发现直接读写这些节点的代码。另有 `/sys/touchscreen/game_para_adjust` 的 `FileReader`，以及 `/sys/class/tshell/tshell_game` / `tshell_shell` 的读取逻辑（`com.vivo.common.a.c2`、`com.vivo.common.e.b`），它们用于能力/状态读取，不应误写为调度下发。

## 4. GameWatch / GameLib / GameKit / GameSpace 调用链

### 4.1 GameCube -> GameWatchManager（被调方是 GameWatch）

【观察】两个反射封装均加载 `com.vivo.gamewatch.common.GameWatchManager` 并调用静态 `getInstance()`：

- `ya.a.<init>`：`focused/ya_a.txt:30-48`。
- `com.vivo.common.utils.p0.<init>`：`focused/com_vivo_common_utils_p0.txt:54-73`。

随后反射调用 `execute(String,Object[])`（`p0`）或同名 `execute`（`ya.a`）。因此在这些调用点上，GameCube 是调用方，GameWatch 是被调方。

`ya.a.b(Object...)` 以字符串 `"GameLib"` 为 channel 调用 execute，并打印 `start to request gameLib`，见 `focused/ya_a.txt:248-289`。其调用点 `com.vivo.gameassistant.r.m3()` 先注册 `cd.b` 接收器，再传整数 1 请求，见完整反汇编行 3512442-3512453。

【推断】GameCube 不是直接绑定 GameLib 服务请求该功能，而是 **GameCube -> GameWatchManager.execute -> GameLib 下游**；GameLib 的内部实现不在本 APK 内。

### 4.2 GameWatch -> GameCube 的回传

`cd.b.c(Context)` 注册 `com.vivo.gamewatch.game.gamelibInfo` 广播，权限 `com.vivo.permission.REC_GAME_MODE_LIGHT`，见 `focused/cd_b.txt:95-132`。`cd.b$b.onReceive` 读取：

```text
RequestMode, UpdateMode, NeedUpdate, UpdateContent,
ModelUpdateSuccess, DownloadResultCode
```

见 `focused/cd_b_b.txt:42-276`。这是 GameWatch/GameLib 信息返回 GameCube 的静态证据。

其他 GameWatch channel 也已确认：

- `control_panel`：读取/保存 app 配置；
- `info_provide`：获取支持列表和信息；
- `hang_up_game`：挂机开始/停止。

`p0.a()` 是通用 `execute` 入口，`p0.b()` 则反射 `gather(String,Object[])`，见 `focused/com_vivo_common_utils_p0.txt:89-175`。

### 4.3 GameKit

`zh.n$b.c(Context)` 显式构造：

```text
ComponentName("com.vivo.gamekit", "com.vivo.gamekit.GameToolsService")
context.bindService(intent, connection, 1)
```

证据：`focused/zh_n_b.txt:118-163`。日志包括 `GameKit service connected, activeStrategy=...`，调用方是 GameCube，GameKit 是被调方。

【边界】该明确绑定位于 OCR 策略 `zh/n`，不能仅据此宣称性能档位也经由 GameKit。新版档位已另有 `com.iqoo.powersaving` 路径。

### 4.4 GameSpace

`xe.a.c/d()` 显式启动 `com.vivo.game/com.vivo.gamespace.ui.splash.GameSpaceSplashActivity`，携带 `jump_from=com.vivo.gamecube`、`jump_target=GSTgpMatchListActivity`，见完整反汇编行 3803955-3804033。另一个入口使用 `vivogame://game.vivo.com/space/game_space_splash_activity` 或 `vivogamespace://...openjump2`，见行 4838814 和 4838911。

【观察】显式组件的包名是 `com.vivo.game`，不能仅因 APK 文件叫 `vivogamespace.apk` 就把组件包名写成 `com.vivo.gamespace`。

## 5. 悬浮窗/侧边栏如何触发性能请求

静态调用链如下：

1. `EdgeDockView` 在 `STATE_PERFORMANCE` 下记录 `onPerformanceAreaClick` 并对性能 RadioButton `performClick()`，见完整反汇编行 3879726-3879738。
2. `qf.c.enterPerformancePanel()` 创建/复用 `PerformancePanelManager` 并调用 `e()`，见行 4858694-4858714。
3. `PerformancePanelManager.e()` 创建 `PerformancePanelPanelView`，通过 `hf/b.e(view, WindowFragmentParams)` 展开新窗口；视图初始化 `ModeShiftBar` 并设置点击监听，见 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_common_panel_PerformancePanelManager.txt:257-327` 和 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_common_panel_PerformancePanelPanelView.txt:223-295`。
4. `ModeShiftBar` 调用 presenter `q.d/e/h(ModeType)`。
5. `q` 调 `m.D0/J0/x0`，最终进入 `ModeShiftHelper.w(ModeType)`。
6. `w()` 向 `com.iqoo.powersaving` 发送第 3.1 节的 command。

即：**侧边栏性能入口 -> 性能面板 -> ModeShiftBar -> presenter/controller -> POWER_MODE_CHANGE_SERVICE**。侧边栏本身没有直接写内核节点。

GameWatch 事件也能影响面板：`modeshift/f.H()` 注册 `com.vivo.gamewatch.game.targetfps` 和 `quality`，随后根据游戏帧率/画质提示用户切换 MONSTER 或 ENHANCED，见 `focused/com_vivo_gameassistant_homegui_sideslide_panels_performance_modeshift_f.txt:231-317`。这是“提示/引导切换”，不是另一条直接内核请求。

【未知】本任务遵守只读要求，未做 UI 点击或运行时 Binder 跟踪，因此上述是静态可达调用链，不是一次实测事件时序。

## 6. 结论速查与未知项

- GameCube 是游戏设置/侧边栏与多组件路由中心，不是 `/sys/rsc` 的直接写入者（在本 APK 的完整 DEX 中未命中）。
- 游戏识别以名单数据库和安装校验为主，统一配置可更新；功能白名单与“是否游戏”分开处理。
- 新版性能档位只下发 `command`：均衡 1004、省电 1005、Monster 1011、Enhanced 1014，`source=23`，目标 `com.iqoo.powersaving`。
- 按游戏记忆不是独立 custom 档；旧 `game_cube_gpu_mode=3` 才是自定义 GPU Profile，走 QTI ProfileManager/JNI。
- GameWatch 是 GameLib 请求的被调方和回传广播来源；GameKit 是 OCR 等策略的被调服务；GameSpace 由显式 Activity 跳转打开。
- 下游 command 到内核参数的映射、外部 `profilemanager-jni` 实现、统一配置线上内容、旧 GPU mode 1/2 的更精确命名均未在本 APK 内解决。
