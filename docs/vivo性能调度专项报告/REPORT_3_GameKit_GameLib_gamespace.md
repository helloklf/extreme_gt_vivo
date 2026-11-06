# 方向 3：GameKit / GameLib / gamespace 静态逆向报告

## 1. 范围、方法与结论边界

目标 APK：

- `analysis/device_pull/perf/GameKit.apk`
- `analysis/device_pull/perf/GameLib.apk`
- `analysis/device_pull/perf/vivogamespace.apk`
- 为验证组件关系，少量交叉检查了同目录 `GameCube.apk`，没有展开其他方向的分析。

全程只进行本地静态分析，没有启动 APK、注入进程，也没有修改设备节点、属性或文件。本文把直接命令输出、清单和 smali/ELF 证据标为“观察”，由这些证据得到的角色判断标为“推断”。未做运行时验证。

工具情况：

- `which jadx jadx.bat apktool` 均无结果；REA Android 后端在当前 `win32/arm64` 上返回 `unsupported_provider`，ELF 布局后端也只支持 Linux x64。
- 使用本机已有 `C:/Users/hello/apktool/apktool.jar`（Apktool 2.11.1）生成 smali/资源。
- GameKit 和 GameCube 因缺少 package id 3 的 vivo framework，Apktool 在资源阶段报错，但 smali 已生成；其清单改用 `aapt2 dump badging/xmltree` 读取。GameLib、gamespace 完整解码成功。
- native 使用 NDK 26.3.11579264 自带 LLVM 17.0.2 的 `llvm-readelf/llvm-nm/llvm-objdump/llvm-strings`。

主要命令：

```bash
sha256sum analysis/device_pull/perf/{GameKit,GameLib,vivogamespace,GameCube}.apk
java -jar /c/Users/hello/apktool/apktool.jar d -f -o <输出目录> <APK>
/d/Android/SDK/build-tools/36.0.0/aapt2.exe dump badging <APK>
/d/Android/SDK/build-tools/36.0.0/aapt2.exe dump xmltree --file AndroidManifest.xml <APK>
```

APK 身份（`sha256sum` 与 `aapt2 dump badging` 观察）：

| APK | package / version | SHA-256 |
|---|---|---|
| GameKit | `com.vivo.gamekit` / `1.0.05.01` (100501) | `9b6a3789872d3de7ccf6ec3c90bdb8f26662df1e13f295bb1338505b1b934af6` |
| GameLib | `com.vivo.gamelib` / `17013.0` (17013) | `b5673d34935b7cf3cffc92ef30838babf9f3cfecff4002aa79156bef5521e17e` |
| gamespace | `com.vivo.gamespace` / `1.0.4.3` (10043) | `09b4c82093effa13d0a8571bcaac2528c998c15eb148f5da7219cf1946dcb88d` |
| GameCube（交叉证据） | `com.vivo.gamecube` / `15.0.06.02_de` (1500602) | `2fe0c05983991b1b829918ccbc2f49658409fab4a259ec4330bdd894408370ba` |

## 2. 总结论

| 包 | 有证据支持的角色 | 对初步假设的修正 |
|---|---|---|
| GameKit | **签名权限保护的游戏 AI 工具服务**：截图 OCR/图像识别、结果回调、模型和自身升级。 | 不是已证实的三方游戏性能 SDK；其导出 AIDL 不含 FPS/CPU/GPU/调频申请。 |
| GameLib | **进程内 Java/JNI 图形功能库**：QV1 超分、MEMC 插帧、VNSS、游戏录制/截图；经 GameWatchManager 和 GameManager 与系统服务衔接。 | 确实含大型 native 层并转发到 GameWatch，但转发内容是图形功能命令，不是已证实的 CPU 性能申请。没有独立 Android Service。 |
| gamespace | **游戏空间/游戏中心 UI 与本地游戏聚合入口**：游戏列表、详情、下载、成长体系、使用统计，并启动 GameCube。 | “gamespace 是入口、GameCube 是游戏模式功能实现包”得到静态代码支持；GameCube 也不是唯一底层通道，GameLib 可直接调用 GameWatchManager。 |

对“游戏进程如何声明/请求性能”的回答仅限于这三个包：**未发现三方游戏通过 manifest、AIDL、broadcast 或 provider 声明/申请 CPU 性能的接口。GameLib 的图形功能采用宿主包名 + 白名单/设置开关，而不是游戏自行声明性能模式。** 真正的通用性能 SDK 如果存在，需要到 GameWatch、framework 的 `com.vivo.game.GameManager` 或其他系统组件继续查找。

## 3. GameKit：游戏 AI 工具服务，不是性能申请 SDK

### 3.1 清单与导出边界（观察）

`aapt2 dump xmltree --file AndroidManifest.xml GameKit.apk` 的关键输出：

```text
package="com.vivo.gamekit"
E: permission
  android:name="com.vivo.gamekit.permission.REQUEST_GAMETOOLS"
  android:protectionLevel=0x00000002
E: service
  android:name="com.vivo.gamekit.GameToolsService"
  android:permission="com.vivo.gamekit.permission.REQUEST_GAMETOOLS"
  android:exported=true
  E: action android:name="com.vivo.gamekit.action.GAME_TOOLS"
```

`protectionLevel=2` 即 signature 级。它的 provider 只有升级用 `com.vivo.gamekit.upgrade` 和 AndroidX startup，均 `exported=false`。没有面向三方游戏的性能 ContentProvider。

`GameToolsService.onBind()` 返回 `LB0/a` Binder；服务 `onCreate()` 注册 `_OCR` 与 `_GRAPHICS_IMAGE` 模型：

- `analysis/device_pull/perf/apktool/GameKit/smali/com/vivo/gamekit/GameToolsService.smali:31`：`onBind` 返回 `c:LB0/a`。
- 同文件 `:62-121`：`ModelUpgradeHelper.registerModels`，模型名为设备后缀加 `_OCR`、`_GRAPHICS_IMAGE`。
- 同文件 `:213`：释放时调用 `GokModelSet.releaseModel()`。

### 3.2 AIDL 方法及真实功能（观察）

GameKit APK 本身未打包 `.aidl` 文本，但 GameCube APK 内含原始接口；服务端 Binder 描述符完全一致。提取命令：

```bash
unzip -p analysis/device_pull/perf/GameCube.apk com/vivo/gamekit/IGameTool.aidl
unzip -p analysis/device_pull/perf/GameCube.apk com/vivo/gamekit/IGameToolCallback.aidl
```

关键原文：

```aidl
interface IGameTool {
    String sendScreenCaptureSync(String sceneJsonValue, in SharedMemory sharedMemory);
    void sendScreenCaptureAsync(String sceneJsonValue, in SharedMemory sharedMemory,
                                IGameToolCallback callback);
    boolean requestUpdate(String packageName, IGameToolCallback callback,
                          String requestParam);
}
interface IGameToolCallback {
    void onScreencapParsed(String result);
    void onAlgoUpdated(int retCode);
}
```

服务端证据：

- `analysis/device_pull/perf/apktool/GameKit/smali/B0/a.smali:21-23`：`attachInterface(..., "com.vivo.gamekit.IGameTool")`。
- 同文件 `:963-998`：transaction 1 对应同步截图接口，并记录 `sendScreenCaptureSync: deprecated, return null`。
- 同文件 `:171-234`：transaction 2 读取 JSON、`SharedMemory` 和 `IGameToolCallback`。
- 同文件 `:255-318`：JSON 键为 `query_type`、`package_name`；`:320`、`:454` 识别 `init_model`、`destroy_model`；`:553-612` 把共享内存解码为 Bitmap；`:710-899` 分发高亮/助手识别任务。
- 同文件 `:91-169`：transaction 3 是 `requestUpdate`，进入 `VivoUpgradeClient.checkUpgrade`。
- `analysis/device_pull/perf/apktool/GameKit/smali/B0/b.smali:14-48`：callback transaction 1 回传字符串识别结果。
- `analysis/device_pull/perf/apktool/GameKit/smali/G0/j.smali:1105-1153`：callback transaction 2 回传算法升级整数结果。

GameKit 没有打包 `.so`（`unzip -l GameKit.apk | awk '/\.so$/'` 无输出），但 Java 会加载系统 AI 库：

- `analysis/device_pull/perf/apktool/GameKit/smali/com/vivo/aigametool/OcrDetectRec.smali:15-29`：`vcap_core`、`vcap_opencl`、`ocrPostMatch`。
- `analysis/device_pull/perf/apktool/GameKit/smali/com/vivo/ai/RecognizeSkillYs.smali:7-52`：`SkillDetectYs` 及 `recognizeBP/recognizeYS` 等 native 识别方法。

**推断：** GameKit 是游戏助手 AI 能力的服务端，接口用于把游戏截图交给识别模型并更新模型/代码，不是三方游戏申请性能调度的 SDK。签名权限也说明它不是开放给普通三方应用的公共 Binder API。

### 3.3 GameCube 是否为调用方（观察与限制）

GameCube 清单请求了 `com.vivo.gamekit.permission.REQUEST_GAMETOOLS`，且包内含上述 AIDL；这证明它具备接入该接口的声明和类型材料。但在 GameCube 全部 smali 中检索：

```text
com.vivo.gamekit.action.GAME_TOOLS
com.vivo.gamekit.GameToolsService
sendScreenCaptureAsync / sendScreenCaptureSync / onAlgoUpdated / requestUpdate(
```

均未找到直接调用。故**不能仅凭 AIDL 文件就断言此版本 GameCube 实际绑定 GameKit**；也可能由间接封装、动态代码或历史遗留材料造成，当前未解决。

对 GameKit 全部 smali 检索 `com.vivo.gamewatch|com.vivo.gamelib|com.vivo.gamecube|com.vivo.game.GameManager|/sys/rsc|vivo_rsc`，无匹配；这仅是上述静态搜索边界内的否定结果。

## 4. GameLib：进程内图形 Java/JNI 库及 GameWatch 转发链

### 4.1 包装形态（观察）

`analysis/device_pull/perf/apktool/GameLib/AndroidManifest.xml:2-3`：

```xml
<manifest package="com.vivo.gamelib" ...>
    <application android:allowBackup="false" android:extractNativeLibs="false"
        android:forceQueryable="true" android:label="GameLib"
        android:multiArch="false" android:usesNonSdkApi="true"/>
</manifest>
```

没有 activity/service/receiver/provider，也没有 AIDL 文件。`GameCameraService`、`EcoSdkServiceQV1` 虽名为 Service，但 superclass 都是 `java.lang.Object`，不是 Android Service：

- `analysis/device_pull/perf/apktool/GameLib/smali/com/vivo/gamelib/gamecamera/GameCameraService.smali:1-7`
- `analysis/device_pull/perf/apktool/GameLib/smali/com/vivo/gamelib/ecosdk/EcoSdkServiceQV1.smali:1-3`

可见的进程内入口包括：

- `GameLibManager.getInstance()/init()`，静态加载 `GameLib_jni`：`analysis/device_pull/perf/apktool/GameLib/smali/com/vivo/gamelib/GameLibManager.smali:33-46`、`:65-109`。
- `EcoSdkManagerQV1.getInstance()`：同目录 `ecosdk/EcoSdkManagerQV1.smali:3490-3529`。
- `GameCameraManager.getInstance()`：同目录 `gamecamera/GameCameraManager.smali:3509-3548`。
- `QV1SurfaceView/GCSurfaceView` 与 Activity 生命周期回调，用于 Surface 捕获和渲染。

两个 Manager 都通过 `ActivityThread.currentApplication()` 取当前宿主 Application 和 `getPackageName()`：

- `EcoSdkManagerQV1.smali:976-1005`
- `GameCameraManager.smali:876-898`

**推断：** GameLib 是被注入或由系统框架加载到目标游戏/宿主进程的库，不是独立服务 APK。具体由谁完成 DEX 加载/注入，单凭该 APK 无法确定。

### 4.2 游戏识别和白名单（观察）

GameLib 不靠游戏声明性能，而是读取宿主包名并做两层识别：

1. 反射 `com.vivo.game.GameManager.getInstance()`，取得 `isWhitelistApp(int)` 和 `loadGameLibraryExt(String)`：
   - `EcoSdkManagerQV1.smali:4195-4308`。
   - `GameCameraManager.smali:4659-4748` 还反射 `sendMessage(String, Message)`。
2. 内置特定游戏包名和配置：
   - `EcoSdkManagerQV1.initIsSpecificGame()` 识别永劫无间、原神、鸣潮、燕云十六声等变体：`EcoSdkManagerQV1.smali:4475-4651`。
   - `EcoSdkPowerOptQV1` 对 `com.miHoYo.Yuanshen`、`com.miHoYo.hkrpg`、`com.kurogame.mingchao` 保存低分辨率参数：`analysis/device_pull/perf/apktool/GameLib/smali/com/vivo/gamelib/ecosdk/EcoSdkPowerOptQV1.smali:244-329`。
   - `GameCameraManager` 另有无限暖暖、绝区零等包名列表：`GameCameraManager.smali:4865-5095` 附近。

`EcoSdkPowerOptQV1` 监听 `app_low_res_enabled`、`app_sr`，反射系统 `com.vivo.superresolution.VivoSuperResolutionManager.onAppSRChanged(boolean,int,int,boolean,long)`：同文件 `:156-200`、`:405-487`、`:585-706`。这里的“PowerOpt”是降低应用渲染分辨率，不是 CPU 提频。

### 4.3 到 GameWatchManager 的明确 Java 转发（观察）

两个 Manager 均反射同一系统接口：

```text
com.vivo.gamewatch.common.GameWatchManager
getInstance()
execute(String, Object[])
```

证据位置：

- `EcoSdkManagerQV1.initGameWatchManager()`：`EcoSdkManagerQV1.smali:4389-4473`。
- `EcoSdkManagerQV1.lambda$execute$4()`：同文件 `:5242-5288`，实际 `Method.invoke(mGameWatchManager, {json, args})`。
- `GameCameraManager.initGameWatchManager()`：`GameCameraManager.smali:4775-4858`。
- `GameCameraManager.lambda$execute$0()`：同文件 `:5541-5587`。

JSON tag 和命令不是 CPU 调度，而是图形状态：

| tag | 命令/数据 | 证据 |
|---|---|---|
| `game_messenger` | `ecosdk_setMessenger`，同时传 Messenger | `EcoSdkManagerQV1.smali:6049-6080` |
| `game_messenger` | `gamecamera_setMessenger`，同时传 Messenger | `GameCameraManager.smali:6281-6312` |
| `ecosdk` | `disable_gpu_memc` | `EcoSdkManagerQV1.smali:6003-6030` |
| `ecosdk` | `all_surface_ready` + `status` | 同文件 `:5911-5939` 起 |
| `ecosdk` | `set_rotation` + `rotation` | 同文件 `:6233-6263` |
| `ecosdk` | `vnss_capture` + `status` | 同文件 `:6320-6350` |
| `ecosdk` | `gamesdkexception`、`retry` | 同文件 `:6099-6137`、`:6188-6214` |

GameLib 设置键也直接对应 GameCube 图形功能：`gamecube_super_resolution`、`gamecube_frame_interpolation`、`gamecube_frame_interpolation_for_sr`，见 `EcoSdkManagerQV1.smali:7051-7081` 及对应 observer。

反向控制通过 Messenger，而不是 manifest Service：

- `GameCameraService.onMessage()` 接收 `0x65`（设置捕获模式）、`0x66`（竖屏）、`0x67`（隐藏 UI）、`0x68`（请求截图）、`0xc8`（场景模式）等，Bundle 键 `id/mode/params`：`GameCameraService.smali:126-273`。
- `GameCameraService.setReply()` 回传 `support_game_capture_mode`、`support_portrait`、`fps_settings_support` 和触摸区域：`GameCameraManager.smali:7532-7588`。
- `EcoSdkServiceQV1` 的 `what=1` 接收 `top/middle/bottom` 边界：`EcoSdkServiceQV1.smali:65-112`。

**结论：** GameLib → GameWatchManager 的 Java 转发链是静态证据闭环；这些命令表达超分/插帧/录制状态，不应解释为 `/sys/rsc` 性能写请求。

### 4.4 native 库与 GameManager 通道（观察）

`unzip -l GameLib.apk` 显示 9 个 arm64 `.so`：

```text
libGameLib_jni.so             41,544
libMEMCSDK.so                852,656
libVNSSSDK.so              1,216,168
libVSR.so                 11,126,560
libVkLayer_VSR.so         12,069,344
libc++.so                  1,152,760
libvivogamecore_legacy.so  2,761,184
libvivogamecore_qv1_gles.so 2,569,856
libvivogamecore_qv1_vulkan.so 3,483,088
```

JNI 证据：

- `GameLibManager.<clinit>` 加载 `GameLib_jni`，两个 Manager 声明 `nativeInit/nativeInitVNSS/nativeSet*`。
- `llvm-nm -D --defined-only libGameLib_jni.so` 只有动态导出 `0000000000003020 T JNI_OnLoad`。
- `llvm-objdump -d --disassemble-symbols=JNI_OnLoad libGameLib_jni.so` 在 `0x314c-3154` 可见经 `JNIEnv` 偏移 `0x6b8` 调用 `RegisterNatives`，`w3=0x15`；与 smali 的 native 方法及字符串表中的 `com/vivo/gamelib/...`、`nativeSet*` 相互印证。
- `nativeInit` 的两个参数来自 `GameManager.loadGameLibraryExt("libVSR.so")` 和 `("libVkLayer_VSR.so")` 返回的句柄：`EcoSdkManagerQV1.smali:1124-1150`；GameCamera 同样在 `GameCameraManager.smali:917-930`。

`llvm-readelf -d` 显示 `libMEMCSDK.so`、`libVNSSSDK.so`、`libVSR.so`、`libVkLayer_VSR.so` 都直接 `NEEDED libgamemanager_aidl.so`。这证明 native 还存在到系统 GameManager 的 Binder/AIDL 支持库边界，而不是所有工作都经过 GameWatchManager。

`llvm-nm -D --undefined-only` 的关键未解析符号：

```text
libVSR.so: android::GameManager::reportResult(...)
libVSR.so: android::GameManager::setVSRThread(...)
libVSR.so: android::GameManager::isVSRWhitelistApp()
libVSR.so: android::GameManager::registerVSREnabledListener(...)
libVkLayer_VSR.so: android::GameManager::reportResult(...)
libVkLayer_VSR.so: android::GameManager::registerVSREnabledListener(...)
libVNSSSDK.so: android::GameManager::getPackageName()
```

字符串/符号也明确其算法用途：`Vsr_Init`、`vfi_drawInterpolatedFrame`、`VNSSWrapper_executeVNSS`、`GLESRendererQV1_*`、`VulkanRendererQV1_*`、`GameCameraGL/VK_*`。

**结论：** native 层是 GLES/Vulkan 超分、插帧、VNSS 和捕获实现，并通过 `libgamemanager_aidl.so` 与系统 GameManager 交互。GameWatchManager 转发是在 Java 层完成的，不要把两条通道混为一条。

### 4.5 未证实到内核的路径

对 GameLib Java 检索 `requestPerformance|setPerformance|performanceMode|performance_mode|/sys/rsc|vivo_rsc|/sys/|fas_|svp_|qos_|ktop`，无匹配；对全部 9 个 `.so` 的可打印字符串检索 `/sys/|vivo_rsc|/rsc|fas_|svp_|qos_|ktop|gamewatch|gamecube`，无有效命中。该否定仅限这些静态字符串和 smali，不能证明运行时一定没有间接内核访问。

当前证据不能回答：`GameWatchManager.execute` 之后如何进入内核、`libgamemanager_aidl.so` 的服务端实现、以及是否有 native ioctl 直接落到 vivo_rsc。需要方向 1/2 的 GameWatch/framework/native 证据继续闭合。

## 5. gamespace：游戏聚合入口与 GameCube 的关系

### 5.1 UI/入口角色（观察）

`analysis/device_pull/perf/apktool/vivogamespace/AndroidManifest.xml`：

- `:88`：Application 为 `com.vivo.game.cover.GameCoverApplication`，label 为 `vivo GameSpace`。
- `:91-98`：`GameSpaceHostActivity` 和导出的 `GameSpaceSplashActivity`，taskAffinity 为 `com.vivo.game.GameSpace`；Splash 接收 `android.intent.action.MAIN`。
- `:129-145`：导出的 `OpenJumpActivity/OpenJumpActivity2`，deep link 为 `vivogamespace://game.vivo.com/openjump[/2]`。
- `:108-116`：`ZGameAIKeyService` 接收 `vivo.settings.JOVI_KEY_SETTINGS_ACTION`。
- `:146-165`：`GameAbeReceiver/GameReceiver` 接收标准及 vivo 的 `PACKAGE_ADDED/PACKAGE_REMOVED`。
- 还声明大量游戏详情、下载、媒体、成长体系 Activity；provider 均非导出，不是性能 SDK provider。

### 5.2 包名识别与本地游戏列表（观察）

本地游戏识别不是简单把所有安装包都当游戏：

1. `PackageInfoManager`（`com.vivo.game.core.c1`）维护包名到 `PackageInfo` 的缓存：`analysis/device_pull/perf/apktool/vivogamespace/smali_classes2/com/vivo/game/core/c1.smali:276-482`、`:485-641`。
2. `GameUsageManager.d()` 从缓存筛选：
   - 排除空包名、`isExternal` 标记项；
   - 排除以 `com.android`、`com.vivo`、`com.bbk` 开头的包；
   - 但 `com.vivo.quickgamecenter`、`com.vivo.minigamecenter` 是例外，可进入候选；
   - 再排除 `Lo6/b.f` 为 true 的项。
   证据：`analysis/device_pull/perf/apktool/vivogamespace/smali_classes3/ha/a.smali:1777-1979`。
3. `LocalGameManager` 结合 Room 游戏数据库、服务器游戏详情和 `UsageStatsManager` 的使用时长/最近使用时间：
   - `analysis/device_pull/perf/apktool/vivogamespace/smali/ub/l.smali:28-375`、`:993-1804`。
   - `GSLocalGame.queryLocalGames` 协程入口：`analysis/device_pull/perf/apktool/vivogamespace/smali/com/vivo/gamespace/ui/main/biz/GSLocalGame.smali:147-347`。
4. 安装/卸载广播把包名及 `REPLACING` 状态交给后续更新：`analysis/device_pull/perf/apktool/vivogamespace/smali_classes3/com/vivo/game/model/GameReceiver.smali:281-371`、`:683-715`。

**推断：** gamespace 负责“哪些已安装包显示为游戏、如何排序和展示”的聚合逻辑，与 GameLib 的“当前宿主是否支持某项图形功能”的白名单是两套不同判断，不能混用。

### 5.3 启动 GameCube 的直接证据（观察）

gamespace 首页有 `gs_magic_cube_entrance`，支持时才显示：

- `GSHomepageShortcutLayout` 绑定入口并设置点击监听：`analysis/device_pull/perf/apktool/vivogamespace/smali/com/vivo/gamespace/ui/main/homepage/GSHomepageShortcutLayout.smali:231-255`。
- `checkSupportGameCube` 通过 `com.vivo.game.core.c1.h("com.vivo.gamecube") > 0` 判断已安装：`.../GSGameCubeUtil$Companion$checkSupportGameCube$2.smali:229-267`；结果控制入口可见性：`GSHomepageShortcutLayout$init$1.smali:262-359`。
- 点击后显式创建 `ComponentName("com.vivo.gamecube", "com.vivo.gamecube.GameCubeMainActivity")`，携带 extra `parameter` 并 `startActivity`：`analysis/device_pull/perf/apktool/vivogamespace/smali/com/vivo/gamespace/ui/main/homepage/a.smali:61-133`。
- 启动失败时，`getLaunchIntentForPackage("com.vivo.gamecube")` 作为回退：`.../GSGameCubeUtil$Companion$startGameCubeDefault$1.smali:185-215`。
- GameCube 侧 `GameCubeMainActivity.B0()` 读取同一 `parameter` 并用于页面埋点：`analysis/device_pull/perf/apktool/GameCube/smali_classes3/com/vivo/gamecube/GameCubeMainActivity.smali:85-127`。
- gamespace 启动时还读取 GameCube application metadata `is_support_gamecube_eight`：`analysis/device_pull/perf/apktool/vivogamespace/smali/com/vivo/gamespace/ui/splash/GSSplashIntentService.smali:133-182`。

GameCube 不只是空壳：`FrameRateOptimizationFragment.i0()` 反射 `GameWatchManager.execute("info_provide", {3})`，取得帧率优化支持游戏列表：`analysis/device_pull/perf/apktool/GameCube/smali_classes3/com/vivo/gamecube/bussiness/FrameRateOptimizationFragment.smali:123-197`。

**结论：** “gamespace 是游戏空间入口，GameCube 是独立的游戏模式设置/功能实现包”得到验证；两者通过显式 Activity 和包信息/metadata 协作。它不是“gamespace 直接写性能节点、GameCube 只是 UI”的结构。

### 5.4 温度读取不是性能写请求（观察）

gamespace 的 `CommonHelpers.Y()` 遍历 `/sys/class/thermal/`，匹配 `case_therm|board_therm|quiet_therm|mtktsAP|ap_ntc`，再拼出对应 `/temp`：

- `analysis/device_pull/perf/apktool/vivogamespace/smali_classes2/com/vivo/game/core/utils/j.smali:3076-3282`。
- 实际读取函数 `Z(String)` 使用 `FileReader/BufferedReader.readLine()`：同文件 `:3444-3524`。

因此本包确有只读温度证据，但不是 `/sys/rsc` 写入或性能声明。

## 6. 接口清单（按任务要求汇总）

| 类型 | 名称/方法 | 暴露方与限制 | 是否性能申请 |
|---|---|---|---|
| Binder action | `com.vivo.gamekit.action.GAME_TOOLS` | GameKit exported service；需 signature 权限 `REQUEST_GAMETOOLS` | 否，AI 游戏工具 |
| AIDL descriptor | `com.vivo.gamekit.IGameTool` | `sendScreenCaptureSync/Async`、`requestUpdate` | 否 |
| AIDL callback | `com.vivo.gamekit.IGameToolCallback` | `onScreencapParsed`、`onAlgoUpdated` | 否 |
| Java 进程内 API | `GameLibManager`、`EcoSdkManagerQV1`、`GameCameraManager`、`QV1SurfaceView` | GameLib；无 Android 导出组件，宿主内使用 | 图形功能，不是通用 CPU 性能 API |
| JNI | `GameLib_jni` 的 `JNI_OnLoad/RegisterNatives`，`nativeInit/nativeSet*` | GameLib 宿主进程；连接 VSR/VkLayer/VNSS/MEMC | 图形功能 |
| GameWatch 反射 | `GameWatchManager.execute(String,Object[])` | GameLib 与 GameCube 均使用 | 已见图形命令/信息查询，下游未知 |
| GameManager 反射 | `isWhitelistApp(int)`、`loadGameLibraryExt(String)`、`sendMessage(...)` | GameLib 宿主进程 | 功能白名单和库加载 |
| native AIDL 支持 | `libgamemanager_aidl.so` | 被 GameLib 的 4 个图形 `.so` 依赖 | 服务端及是否触核未知 |
| Messenger | `game_messenger`；`ecosdk_setMessenger`、`gamecamera_setMessenger` | GameLib 经 GameWatch 注册双向端点 | 图形状态/截图 |
| gamespace deep link | `vivogamespace://game.vivo.com/openjump[/2]` | exported Activity | 否，UI 跳转 |
| gamespace 广播 | `PACKAGE_ADDED/PACKAGE_REMOVED`（Android/vivo 两套 action） | exported receiver | 否，更新游戏列表 |
| provider | GameKit 升级 provider、gamespace 文件/下载/内部 provider | 均 `exported=false` | 无对外性能 provider |

## 7. 未解决问题（unknowns）

1. **GameLib 的加载/注入源**：谁把 GameLib DEX 加载到各游戏进程、何时调用 `getInstance`，本 APK 没有答案。
2. **通用三方性能 SDK 是否存在**：三个目标包未提供证据；需继续查 GameWatch、`com.vivo.game.GameManager` 的 framework 实现及可能的 gpudrivers 接口，不能由本报告推断为不存在。
3. **到内核的最后一跳**：`GameWatchManager.execute` 和 `libgamemanager_aidl.so` 的服务端不在本任务范围，未证明任何命令最终写 `/sys/rsc`。
4. **GameKit 当前真实客户端**：GameCube 有权限和 AIDL 文件，但静态 smali 未找到绑定/调用点；不能断言它在此版本实际使用 GameKit。
5. **白名单 feature id 的官方语义**：`EcoSdkManagerQV1` 对 201、203、204 等调用 `GameManager.isWhitelistApp`，可观察其用于 VNSS/EMEMC 分支，但 id 的完整官方枚举未在目标包中定义。
6. **gamespace 初始包缓存来源**：已确认筛选、数据库和使用统计逻辑，但 `PackageInfoManager` 初始全量枚举/同步的上游调用未完全展开。
7. **运行时覆盖**：没有执行应用或抓取 Binder/sysfs 轨迹；所有调用关系均为静态观察，动态加载、反射失败分支和实际设备配置可能改变运行路径。
