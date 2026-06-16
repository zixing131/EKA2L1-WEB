# EKA2L1 鸿蒙（HarmonyOS）原生版开发记录

> 时间：2026-06-16
> 目标：在 HarmonyOS NEXT 上做一个**原生** app —— ArkTS/ArkUI 界面 + 用 OpenHarmony NDK
> 交叉编译的 C++ 模拟器核心（封装成 NAPI 模块）。UI 设计对标安卓版（`src/emu/android`）。
> 相关提交：`6d44f9ce2`（初版）、`e49af04b4`（首跑修复）

---

## 0. 与 `build_hos.sh` 的区别（重要）

仓库里**已有**一个 `build_hos.sh`，但它是 **WASM 版套 WebView** ——
把 `build_wasm_hos/` 的 wasm 产物塞进 HarmonyOS 的 ArkWeb 组件里跑。

本次新增的是**真·原生版**，对应脚本 `build_hos_native.sh`：

| | `build_hos.sh`（旧） | `build_hos_native.sh`（本次） |
|---|---|---|
| 核心 | Emscripten WASM | OpenHarmony NDK 交叉编译的 arm64/x86_64 |
| 界面 | HTML/JS in ArkWeb | 原生 ArkTS/ArkUI |
| 渲染 | Canvas/WebGL | XComponent + EGL/GLES |
| 产物 | wasm + html | `entry-default-unsigned.hap` |

---

## 1. 构建脚本 `build_hos_native.sh`

用法：

```bash
./build_hos_native.sh              # ohpm install + hvigorw assembleHap（debug）
./build_hos_native.sh release      # 发布模式
./build_hos_native.sh --configure  # 仅跑原生 CMake 配置（快速试错，不打 HAP）
./build_hos_native.sh --native     # 配置 + 编译原生库（不打 HAP，迭代 C++ 编译错误用）
./build_hos_native.sh clean        # 清理生成目录
```

环境变量覆盖：`DEVECO_HOME`、`OHOS_SDK`、`ABI`（默认 arm64-v8a）、`JOBS`。

**自动探测 DevEco Studio**（默认 macOS app bundle `/Applications/DevEco-Studio.app/Contents`）：
- SDK：`sdk/default`（API 22 / OHOS 6.0.2）
- 原生工具链：`hms/native/build/cmake/hmos.toolchain.bisheng.cmake`（BiSheng/clang）
- cmake/ninja：`openharmony/native/build-tools/cmake/bin/`
- node18 + hvigorw + ohpm：`tools/`

**两段式**：
1. 原生层（`--native`）用上面那条工具链对 `entry/src/main/cpp` 跑 CMake。entry 的
   CMake `add_subdirectory` 整个 EKA2L1 仓库根，根据 `CMAKE_SYSTEM_NAME==OHOS` 走
   OHOS 分支编译核心 + `hos_frontend`，最终链成 `libentry.so`。
2. HAP 层（无参/`release`）用 hvigorw `assembleHap`；DevEco 自己会再驱动一次原生
   CMake（走 `entry/build-profile.json5` 的 `externalNativeOptions`，ABI = arm64-v8a +
   x86_64），所以 HAP 里两个架构都有。

**`stage_assets()`**：打 HAP 前把模拟器运行期资源拷进
`entry/src/main/resources/rawfile/assets/`：
- GLES 着色器 + 默认音色库（`src/emu/drivers/resources/`）
- 兼容数据库（`miscs/compat/*.yml`）
- Symbian DLL 补丁（来自原生构建的 `bin/patch/`，所以**首次先跑一次 `--native`** 才有补丁可拷）

> 注意：`rawfile/assets/` 和 `.cxx-native/` 都是生成物，已在
> `src/emu/hos/entry/.gitignore` 忽略，不提交。

---

## 2. 项目结构（对标安卓）

```
src/emu/hos/                         DevEco Stage-model 工程
├─ AppScope/app.json5                bundleName=com.zixing.eka2l1
├─ build-profile.json5               ABI=arm64-v8a,x86_64；SDK 6.0.2(22)
├─ entry/
│  ├─ build-profile.json5            externalNativeOptions -> cpp/CMakeLists.txt
│  └─ src/main/
│     ├─ ets/                        === ArkUI 界面 ===
│     │  ├─ model/AppItem.ets        app 条目（uid/title/icon:PixelMap）
│     │  ├─ model/DeviceItem.ets     设备条目 + PackageItem
│     │  ├─ service/Emulator.ets     NAPI 门面（单例）：沙箱初始化、拷资源、类型转换
│     │  └─ pages/
│     │     ├─ Index.ets             主页：app 网格+图标 / 设备切换 / 装ROM+应用 / 空状态
│     │     ├─ RunPage.ets           运行页：XComponent 全屏 + FPS + 返回
│     │     └─ SettingsPage.ets      设置：设备列表 / 语言 / 卸载包
│     ├─ cpp/                        === NAPI 桥 ===
│     │  ├─ napi_init.cpp            导出对标 Emulator.java 的全部接口
│     │  ├─ xcomponent_bridge.cpp    XComponent 处理（唯一 include XComponent 头的 TU）
│     │  ├─ napi_helpers.h           JS<->C++ 字符串/数组/buffer 转换
│     │  ├─ CMakeLists.txt           add_subdirectory 仓库根，链 hos_frontend
│     │  └─ types/libentry/Index.d.ts  NAPI 的 TS 类型声明
│     └─ resources/rawfile/assets/   （生成物，忽略）运行期资源
└─ native/                           === C++ 前端静态库 hos_frontend ===
   ├─ include/hos/{state,launcher,thread,emu_window_ohos,input_dialog,ui_bridge}.h
   └─ src/{state,launcher,thread,emu_window_ohos,input_dialog,ui_bridge,platform_stubs}.cpp
```

`src/emu/hos/native/` 基本是 `src/emu/android/app/src/main/cpp` 的移植（namespace
`eka2l1::android` → `eka2l1::hos`）：

- **JNI → 回调**：安卓版调 Java 的地方（退出、输入框、问答框）改成 `ui_bridge.h` 里的
  `std::function` 回调，由 NAPI 层注册。
- **app 图标**：安卓 `get_app_icon` 返回 `jobjectArray`（Java Bitmap[]），这里改成返回
  可移植的 `icon_bitmap`（RGBA 缓冲），NAPI 层再 `image.createPixelMap` 成 PixelMap。
- **窗口**：新增 `emu_window_ohos`（持 XComponent 的 `OHNativeWindow*`）+
  `context_egl_ohos`（drivers 层，标准 EGL，render window 直接用 OHNativeWindow）。

---

## 3. NAPI / XComponent 关键点

- **XComponent 头与 keycode.inc 的枚举冲突**：
  `<ace/xcomponent/native_interface_xcomponent.h>` 在全局定义了 `KEY_TAB`/`KEY_HOME`…
  和 EKA2L1 `drivers/graphics/keycode.inc` 的同名匿名枚举撞车。解决：**所有 XComponent
  代码隔离到 `xcomponent_bridge.cpp`**（它绝不 include 任何 EKA2L1 头），`napi_init.cpp`
  反过来绝不 include XComponent 头，两边用 `xcomponent_bridge.h` 里的纯回调结构对接。

- **XComponent 生命周期**：ArkUI `<XComponent libraryname="entry">` 会让系统把
  `OH_NativeXComponent` 实例挂到 NAPI 模块导出的 `OH_NATIVE_XCOMPONENT_OBJ` 上；
  模块 `Init` 里 `register_xcomponent` 取出它并注册 surface 回调。
  - `OnSurfaceCreated` → `surface_changed(window)` + 第一次 `init_threads`，之后
    `start_threads`。
  - `DispatchTouchEvent` → 把 XComponent 触摸类型映射成安卓式 action（0按/1移/2抬），
    喂给 `touch_screen`。

- **entry 的 CMake 必须 `set(CMAKE_CXX_STANDARD 20)`**：它是独立 `project()`，不继承根
  的 C++20，否则 `napi_init.cpp` 见不到 `std::optional`/`is_enum_v` 等。

---

## 4. OHOS 平台适配（核心改动）

策略：**GLES/ARM/POSIX 路径仿 Android，音频/网络仿 WASM**（都没原生后端）。

`common/platform.h`：
- 新增 `EKA2L1_PLATFORM_OHOS`（检测 `__OHOS__`），归入 POSIX，但**从 UNIX 排除**
  （否则会拉进 X11/GLX 桌面路径）。
- 新增 `EKA2L1_HAS_NATIVE_AV()`：WASM 和 OHOS 为 0。`drivers` 里 ffmpeg/cubeb 相关的
  `#if !EKA2L1_PLATFORM(WASM)` 全部换成这个宏（对 WASM 行为不变，OHOS 复用其 fallback）。

根 `CMakeLists.txt`：检测 `CMAKE_SYSTEM_NAME==OHOS` → 置 `EKA2L1_OHOS`、`add_compile_definitions(__OHOS__=1)`、
强制关脚本（无 OHOS LuaJIT 预编译）、`ARCHITECTURE_AARCH64=TRUE`。
`src/emu/CMakeLists.txt`：OHOS 时构建 `hos/native` 而非 `qt`。

**为 OHOS 排除的依赖**（无可用预编译/不兼容）：

| 依赖 | 原因 | 替代 |
|---|---|---|
| cubeb | 只有 AAudio/OpenSL（安卓），OHOS sysroot 无 ALSA/PulseAudio | null 音频驱动 |
| ffmpeg | 仓库内是预编译静态库，无 OHOS arm64 | 无视频/压缩音频 |
| SDL2 | 原生前端用 XComponent | —— |
| libuv/uvw/uvlooper | OHOS musl 无 `cpu_set_t`/`sched_setaffinity` | null 网络（`overall_null`/`btmidman_null`） |
| miniupnp | 同上无网络 | `upnp_null` |

其他：
- **miniBAE**：`drivers/.../minibae/machine/types.h` 把 OHOS 映射成 `X_PLATFORM X_LINUX`；
  `miniBAE_EMU` target 对 OHOS 加 `-Wno-int-conversion`（旧 C 代码 long↔void* 转换，
  BiSheng clang 会当 error）。
- `is_memory_wx_exclusive()` OHOS 返回 true（走 W^X 兼容路径，同安卓/iOS）。
- `launch_browser`：OHOS 在 `platform_stubs.cpp` 里暂为 no-op。

---

## 5. 首跑（arm64 模拟器）修的 3 个崩溃/流程 bug — commit `e49af04b4`

### Bug 1：启动即 SIGABRT
`napi setDirectory` 收到的是 `filesDir + '/eka2l1.cfg'`（一个**文件**路径），却直接拿去
`chdir` → ENOTDIR，CWD 停在只读的 `/` → `setup_log` 打不开 `EKA2L1.log` →
未捕获的 `spdlog_ex` → abort。
**修**：`chdir` 到 `file_directory(path)`；并把 file-sink 的打开包进 try/catch，
CWD 不可写时退化成只用 console，不再 abort。

### Bug 2：无设备时启动 SIGSEGV
ArkUI `boot()` 无条件调 `getApps()`，但没装设备时 `launcher::alserv` 为 null → 解引用崩。
**修**：`get_apps` / `get_app_icon` / `launch_app` 加 null alserv 守卫。

### Bug 3：装设备报「未找到设备」/ code -8，且装完 app 列表仍空
- `onInstallRom` 只选了一个文件就 `installDevice('', path, false)`，`false` 走
  `install_firmware`/VPL 分支 → 对 `.rom` 返回 -8 `vpl_file_invalid`。
  **修**：改成 ROM+RPKG 两文件流程（选 ROM → `doesRomNeedRpkg` → 需要则再选 RPKG →
  `installDevice(rpkg, rom, true)`）。
- 首次启动 `stage_one` 在 device_manager 为空时**跳过了 `startup()`**，所以装完设备后
  内核/OS 线程从未起来，app 列表永远空。
  **修**：新增 `emulator::bring_up_after_install()` + `boot_first_installed_device()`
  （thread.cpp）+ napi `bootFirstDevice`；ArkUI 在装完且原先 `devices.length===0` 时调它。
  这一套对标桌面版 `main_window::on_new_device_added`（startup + set_device +
  挂载 C/D/E + stage_two + 起 OS 线程）。
- **没用 `rescanDevices` 做这件事**：rescan 会 `dvcmngr->clear()` 再从磁盘重建，可能
  把刚装好的设备删掉（rescan 要求 `roms/<firm>/SYM.ROM` 存在；且固件名大小写有坑——
  安装写小写、rescan 读原始 `firm_name`，大小写敏感宿主上可能 miss，参见 WASM 第 32 轮
  大小写映射 bug）。

---

## 6. 第二轮真机问题（arm64 模拟器）— 装/图标/启动/音频

用户在模拟器实测后报的 4 个问题，分两批修。

### 6.1 装 SIS 后界面不刷新
applist 服务端缓存了注册表，装完不会自动反映。
**修**：新增 `launcher::rescan_apps()`（→ `alserv->rescan_registries(io)`，对标桌面
`force_refresh_applist`）→ napi `rescanApps` → ArkTS `Emulator.rescanApps()`；
`onInstallApp` 装成功后先 `rescanApps()` 再 `refresh()`。

### 6.2 游戏列表不显示图标（ArkUI 响应式）
`AppCell` 原是 `@Builder`，图标异步加载后改 `app.icon` 也不重渲染——keyed `ForEach`
（按 uid）不会重建 key 不变的 item。
**修**：抽成独立 `@Reusable @Component AppCell` + `@ObjectLink app`（AppItem 是
`@Observed`），`app.icon` 一赋值就只重渲那个格子；同时去掉会破坏 `@ObjectLink` 的
`this.apps.slice()`。

### 6.3 图标偏色（R/B 互换）
native 出的是标准 RGBA8888，但 HarmonyOS `image.createPixelMap` 从裸 ArrayBuffer
默认按 BGRA 解读 → R/B 互换。
**修**：`InitializationOptions` 加 `srcPixelFormat: RGBA_8888`（告诉它源是 RGBA，让它
自己转）。

### 6.4 打开游戏崩溃 / 黑屏 —— 图形 + 音频两道关

**第一道：图形线程崩 `glClearDepth` 空指针**
崩溃栈 `glad_debug_impl_glClearDepth+84 → pc 0 Not mapped`（图形线程 `bitmap::init_fb`）。
`glClearDepth` 是桌面 GL 才有的函数，GLES 只有 `glClearDepthf`，所以
`eglGetProcAddress("glClearDepth")` 返回 null。根因：`fb_ogl.cpp`/`graphics_ogl.cpp`
里 3 处 `#if defined(EKA2L1_PLATFORM_ANDROID) || defined(__EMSCRIPTEN__)`（glClearDepthf /
glDrawBuffers）**漏了 OHOS** → 走桌面分支。
**修**：三处都加 `|| defined(EKA2L1_PLATFORM_OHOS)`。（`glReadBuffer` 是 GLES3 核心、
非空，不用改。）

**第二道：音频 `dsp_output_stream_shared::start()` 空指针（崩溃与黑屏同根因）**
崩溃栈 `dsp_output_stream_shared::start()+288`，由 guest 启动音频（`eaudio_dsp_stream_start`）
触发。链路：OHOS 用的 `null_audio_driver.new_output_stream` 返回 **nullptr** → DSP 工厂
因 `aud` 非空返回 `dsp_output_stream_pcm` → `pcm::start()`（继承 `_shared`）调
`aud_->new_output_stream()` 拿到 null → `stream_->start()` 解引用崩。
**「其他游戏黑屏」是同一根因的另一面**：即使不崩，pcm 流的 `data_callback` 永不被拉取
（无真实后端），guest MMF 一直等永不完成的缓冲区 → 卡死黑屏。
**修**（两层）：
1. 把 `null_audio_driver` 从「返回 null 流」改成**返回自走时的静音流**
   （[audio_null.h](src/emu/drivers/include/drivers/audio/backend/null/audio_null.h)）：
   `null_audio_output_stream` 自带 20ms 节拍 pump 线程，定时调 `data_callback` 拉取丢弃
   采样 → MMF 缓冲区正常标记「已播放」，管线持续推进（静音不卡）。驱动**保持非空**，
   dispatch 的 player 路径 `driver->get_suitable_player_types()` 也不会崩——两全。
   （没改成「传 nullptr 驱动」是因为 `eaudio_player_open_url` 会无判空解引用驱动。）
2. 防御：`dsp_shared.cpp` 的 `start()` / `set_properties()` / `real_time_position()`
   三处对 `stream_` 空做保护，任何无后端场景都不再崩。

> 7Days 过了音频这关后若仍崩、且栈指向 eikcoctl/CBase（非 audio），那是已知的中文文本
> 路径老问题（E32USER-CBase 21，西文 5320 ROM 跑中文游戏触发），与本轮无关。

---

## 7. 已知遗留 / 待办

- [ ] **无声音**：null 音频现在是「自走时静音流」（不崩不卡，但没声）。后续接 Audio Kit
  的 `AudioRenderer`（PCM 回灌）出真实声音。
- [ ] **无网络**：null 蓝牙/internet。需要 libuv 的 OHOS 适配或换实现。
- [ ] **无摄像头/传感器/振动**：都走 null 后端。可接 Camera Kit / Sensor / Vibrator Kit。
- [ ] `launch_browser` no-op → 接 `startAbilityByType('browser', { uri })`。
- [ ] 设置页设备改名是占位（需要自定义输入对话框）。
- [ ] dyncom 解释器（arm64 guest 跑在 arm64 host 上没用 dynarmic），性能待观察。
- [ ] **app 退出未自动返回列表**：native `on_app_exit` 回调还没接到 ArkUI（需 napi
  线程安全回调），app 正常退出/崩溃后 RunPage 会停在黑屏。
- [ ] 图标偏色修法基于「裸 buffer 按 BGRA 解读」的推断，若实机反而变蓝则改回。
- [ ] 7Days 等中文文本路径可能仍崩（E32USER-CBase 21，与本端口无关的老问题）。
- [ ] HAP 未签名，安装需在 DevEco 配签名 profile。

---

## 8. 涉及文件汇总

**核心平台层**
| 文件 | 改动 |
|---|---|
| `src/emu/common/include/common/platform.h` | `EKA2L1_PLATFORM_OHOS`、`EKA2L1_HAS_NATIVE_AV()`、UNIX 排除 OHOS |
| `CMakeLists.txt`（根） | OHOS 检测、`EKA2L1_OHOS`、`__OHOS__`、关脚本 |
| `src/emu/CMakeLists.txt` | OHOS 构建 `hos/native` |
| `src/emu/drivers/CMakeLists.txt` | OHOS 分支：EGL+egl_ohos、无 cubeb/ffmpeg/SDL2、miniBAE 警告 |
| `src/emu/drivers/src/graphics/context.cpp` | 接入 `context_egl_ohos` |
| `src/emu/drivers/src/graphics/backend/context_egl_ohos.{h,cpp}` | 新增 OHOS EGL |
| `src/emu/drivers/src/graphics/backend/ogl/graphics_ogl.cpp` | OHOS 用 EGL 加载 GLES |
| `src/emu/drivers/src/audio/{audio,dsp,player}.cpp`、`video/video.cpp` | 改用 `EKA2L1_HAS_NATIVE_AV()` |
| `src/emu/drivers/src/hwrm/vibration.cpp` | OHOS → null 振动 |
| `src/emu/drivers/include/drivers/audio/backend/minibae/machine/types.h` | OHOS → X_LINUX |
| `src/emu/common/CMakeLists.txt`、`src/emu/services/CMakeLists.txt` | OHOS → null 网络/UPnP |
| `src/external/CMakeLists.txt` | OHOS 排除 cubeb/ffmpeg/SDL2/libuv/miniupnp |

**第二轮真机修复（§6）**
| 文件 | 改动 |
|---|---|
| `src/emu/drivers/src/graphics/backend/ogl/fb_ogl.cpp`、`graphics_ogl.cpp` | GLES 守卫加 OHOS（glClearDepthf/glDrawBuffers，修 glClearDepth 空指针崩） |
| `src/emu/drivers/include/drivers/audio/backend/null/audio_null.h` | null 驱动改返回自走时静音流（修音频崩溃+黑屏） |
| `src/emu/drivers/src/audio/backend/dsp_shared.cpp` | `start/set_properties/real_time_position` 对空 `stream_` 防护 |
| `src/emu/hos/native/src/{launcher,state}.cpp`、`include/hos/launcher.h` | `rescan_apps`；启动崩溃/空设备/装机流程修复（§5+§6） |
| `src/emu/hos/entry/src/main/cpp/napi_init.cpp`、`types/libentry/Index.d.ts` | `rescanApps`/`bootFirstDevice` napi 接口 |
| `src/emu/hos/entry/src/main/ets/pages/Index.ets`、`service/Emulator.ets`、`pages/RunPage.ets` | `@Component AppCell`+图标响应式、装后 rescan、icon srcPixelFormat、启动前 setScreenParams |

**鸿蒙工程**：`build_hos_native.sh` + `src/emu/hos/`（整目录）

---

## 9. 经验

1. **OHOS toolchain 把 OHOS 当 `__unix__`**：必须把它从 `EKA2L1_PLATFORM_UNIX` 排除，
   否则会编译进 X11/GLX 等桌面专属路径。模式同安卓（安卓也 define `__unix__`）。
2. **第三方库的平台分支要逐个查**：cubeb（无 OHOS 后端）、ffmpeg（预编译无 arm64）、
   libuv（musl 无 cpu_set_t）、miniBAE（X_PLATFORM 必须外部定义）—— 每个都得显式处理，
   策略统一为「仿 WASM 走 null」。
3. **SDK 头的全局枚举会污染**：XComponent 的 KEY_* 撞 keycode.inc，靠 TU 隔离解决，
   比改任何一边的枚举都干净。
4. **沙箱里 CWD 是只读根**：原生 app 第一件事就是 `chdir` 到可写的 `filesDir`，否则
   任何相对路径写（日志/配置/drives）全失败。日志打开务必 try/catch 兜底。
5. **设备从无到有要显式 bring-up**：桌面/安卓在「空设备列表」下不会 startup，装完第一个
   设备后必须手动 startup+set_device+mount+stage_two+起 OS 线程，不能指望 rescan。
6. **GLES 不是 GL 的子集别假设**：`glClearDepth`（桌面）vs `glClearDepthf`（GLES）、
   `glDrawBuffer` vs `glDrawBuffers`——GLES 缺的函数 `eglGetProcAddress` 返回 null，
   一调就崩。所有 ANDROID/EMSCRIPTEN 的 GLES 守卫都要确认带上 OHOS。
7. **「null 后端」别真返回 null 流**：DSP `_shared`/`_pcm` 假设 `new_output_stream`
   给的是有效流（会 `stream_->start()`），且靠 data callback 被拉取来推进 MMF 缓冲。
   返回 null 既崩又卡。无声平台要给「自走时静音流」（定时拉 callback 丢弃），保持驱动
   非空（player 路径会无判空解引用驱动）。
8. **ArkUI keyed ForEach + 异步数据**：key 不变的 item 不会因数组重赋值而重建；异步填充
   的字段（图标）要走 `@Component`+`@ObjectLink`（配 `@Observed` model）才会重渲染，
   `@Builder` 不行。
9. **裸 buffer 转 PixelMap 的字节序**：HarmonyOS `createPixelMap` 从 ArrayBuffer 默认按
   BGRA 解读，RGBA 源要显式 `srcPixelFormat: RGBA_8888`，否则 R/B 互换。
