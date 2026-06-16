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

## 6.5 第三轮真机崩溃 — 装 ROM 即闪退（错误地选了 Dynarmic JIT）

### 错误：装 ROM 后 `startup()` 起 CPU 核时 SIGSEGV in oaknut/Dynarmic

**现象** 真机（nova 12 Ultra，arm64，ADL-AL00U 6.1.0）装完 ROM 立刻闪退，进程仅存活 3s。
`Reason:Signal:SIGSEGV(SEGV_MAPERR)@0xffffffffffffffff`。崩溃栈：

```
#16 hos::emulator::stage_one()
#14 system_impl::startup()
#13 arm::create_core(..., arm_emulator_type)
#11 arm::dynarmic_core::dynarmic_core(...)         ← 居然创建了 Dynarmic 核
#10 arm::make_jit(...)
#06 Dynarmic::A32::Jit::Impl::Impl(...)
#04 Dynarmic::Backend::Arm64::A32AddressSpace::EmitPrelude()   ← 往 JIT 缓冲写机器码
#02 oaknut::BasicCodeGenerator::LDR(...)            ← oaknut（Dynarmic 的 arm64 汇编器）
#00 oaknut::PointerCodeGeneratorPolicy::append(uint)  ← 写代码缓冲越界
```

**根因（两层对「ARM」的定义不一致，aarch64 在两边都漏判）**
本意是「OHOS arm64 用 dyncom 解释器、不用 dynarmic」（§7 待办里也这么写），但该意图**从未生效**：

1. **C++ 选核**（[epoc.cpp](src/emu/system/src/epoc.cpp) `system_impl` 构造）用
   `#if EKA2L1_ARCH(ARM)`，而 `EKA2L1_ARCH_ARM` 在 [platform.h](src/emu/common/include/common/platform.h)
   **只对 32 位 `__arm__` 定义**，aarch64 走的是 `EKA2L1_ARCH_ARM64`。于是 OHOS arm64 上
   `EKA2L1_ARCH(ARM)` 为假 → 跳过 r12l1 → 跳过 WASM → 落到 `#else` → `cpu_type = dynarmic`。

2. **CMake 选库**（[src/emu/cpu/CMakeLists.txt](src/emu/cpu/CMakeLists.txt)）只判
   `EMSCRIPTEN` / `ARCHITECTURE_ARM32` / `else`，OHOS 落 `else` → 链接 Dynarmic。根 CMake
   设的 `ARCHITECTURE_AARCH64 TRUE`（注释自称"force ARM target so dynarmic is skipped"）
   **根本没人消费**——[src/external/CMakeLists.txt](src/external/CMakeLists.txt) 还用
   `check_symbol_exists(__aarch64__)` 把它覆盖回去，dynarmic 照样进编译。

两层一致地选错：编译 Dynarmic + 运行时也请求 Dynarmic。而 Dynarmic 是 host JIT，需要
可执行内存。

**关键政策细节：鸿蒙的 JIT「开发可用、上架不可用」**
鸿蒙同 iOS：**开发/调试签名的 app 能拿到可执行内存（JIT 可跑），但上架商店的签名 app
被沙箱拒绝 `PROT_EXEC`**。所以不能简单「OHOS 一律 dyncom」（那样开发机白白丢掉 JIT 性能），
也不能「OHOS 一律 dynarmic」（上架即崩）。正解是**运行期探测 + 自动降级**。

**修复（运行期探测可执行内存，能 JIT 就 JIT，不能就降级 dyncom）**
- [virtualmem.cpp](src/emu/common/src/virtualmem.cpp) 新增
  `is_executable_memory_available()`：`mmap` 一页 → 写入一条 `ret`（aarch64 `0xD65F03C0`）
  → `mprotect` 到 `PROT_READ|PROT_EXEC` → `__builtin___clear_cache` 同步 i-cache → 真正
  调用一次。成功才返回 true（既测「PROT_EXEC 被拒」也测「给了但不可执行」）。结果缓存。
  这条 RW→RX 路径与 Dynarmic 内部 W^X 分配方式一致，故探测结果能代表 Dynarmic 能否工作。
- [epoc.cpp](src/emu/system/src/epoc.cpp)：OHOS 单独一个 `#elif` 分支，
  `cpu_type = is_executable_memory_available() ? dynarmic : dyncom`。WASM 仍恒 dyncom。
- [virtualmem.cpp](src/emu/common/src/virtualmem.cpp) `is_memory_wx_exclusive()` 把 OHOS
  加入返回 true 的列表（同 iOS/Android）：JIT 真被启用时走 W^X 安全的 RW↔RX 翻转，不申请 RWX。
- **CMake 不动**：OHOS 是真 aarch64，`ARCHITECTURE_ARM32` 为假 → 本来就会
  `add_subdirectory(dynarmic)` 并链接 Dynarmic 的 arm64 后端（崩溃栈里的
  `Dynarmic::Backend::Arm64` 证明它已为 arm64 编出来了）。dynarmic 留在构建里给开发机用。
- `arm_factory.cpp` 的 dynarmic include / case 守卫维持 `!EKA2L1_PLATFORM(WASM)`（OHOS 要
  编 dynarmic case）。

> 上架的 store 构建探测失败 → dyncom（纯解释器，arm64 偏慢但不崩）；开发机/侧载探测成功
> → dynarmic JIT（快）。同一个 HAP 二进制两种环境自适应，无需两套产物。

### 教训

1. **CMake 变量 ≠ C++ 预处理宏**：根 CMake 里 `set(ARCHITECTURE_AARCH64 TRUE)` 不会定义
   `EKA2L1_ARCH_ARM` 宏，也不会被 `src/external` 的 `check_symbol_exists` 之后的逻辑当真
   （它紧接着就被覆盖）。想影响选核必须落到真正被消费的那个开关上。
2. **`EKA2L1_ARCH(ARM)` 只表示 32 位 ARM**，aarch64 是 `EKA2L1_ARCH(ARM64)`。任何
   `#if EKA2L1_ARCH(ARM)` 的分支在 arm64 host 上都为假，写跨平台分支时要把 aarch64 显式
   考虑进去（W^X JIT 选择尤甚）。
3. **「能不能 JIT」是运行期问题、不是编译期问题**：鸿蒙/iOS 同一个二进制，开发签名能 JIT、
   商店签名不能。编译期 `#if PLATFORM(OHOS)` 无法区分这两种运行环境，硬编码任一边都错。
   正确做法是运行期探测一次（mmap+mprotect+执行一条指令）再选后端——探测路径要和真正的
   JIT 分配方式一致才有代表性。

---

## 6.6 第四轮真机崩溃 — dispatcher trampoline chunk 用了 RWX（W^X 拒绝）

承接 §6.5：dynarmic 那关过了之后（新构建 hash `052e12be`，build-id 与崩溃日志一致），
装 ROM 仍闪退，但崩点完全变了。

### 错误：`set_device → reset → setup_outsider → dispatcher 构造` 里 chunk 清零写崩

**现象** `SIGSEGV(SEGV_ACCERR)@0x5cfe7ce000`，进程 3s。完整栈（符号化后）：

```
#13 hos::emulator::stage_one()
#11 system_impl::set_device()
#10 system_impl::reset()
#09 system_impl::setup_outsider()
#07 dispatch::dispatcher::dispatcher(kernel_system*, ntimer*)
#06 kernel_system::create<kernel::chunk>(...)
#05 make_unique<kernel::chunk>(...)            dispatcher.cpp
#04 kernel::chunk::chunk(...)
#03 std::fill<unsigned char*>(...)             ← chunk.cpp:151 清零写
#00 std::__fill_n<unsigned char*>
```

寄存器决定性：`x0=x9=0x5cfe7ce000`（写目标=fault 地址）、`x1=0x4000`（写 16KB）、
`esr=0x92000047`（数据写权限错）。`0x4000` = `MAX_TRAMPOLINE_CHUNK_SIZE`。

**根因（又是 W^X，换了个地方）**
[dispatcher.cpp](src/emu/dispatch/src/dispatcher.cpp) 构造时建 trampoline chunk 用了
**`prot_read_write_exec`（RWX）**。chunk 创建后 [chunk.cpp:151](src/emu/kernel/src/chunk.cpp)
`std::fill(base..., clear_byte)` 清零。OHOS（同 iOS）W^X：`commit` 的
`mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` 被内核拒绝 → 页留在不可写 → 清零写第一字节就
`SEGV_ACCERR`。这就是 §6.5 修完 dynarmic 后「往后多走了几步又崩」的下一颗雷。

**关键观察**：trampoline chunk 的内容是**宿主写入、guest CPU 执行**——host 侧
`start_base[0]=0xEFC10001…` 写的是 guest ARM 指令，由模拟 CPU（dyncom 解释 / dynarmic
重编译进自己独立的可执行缓冲）当数据读取,host **从不直接执行**这块内存。所以这里的
PROT_EXEC 对 host 是多余的,RW 足矣。

**修复**（[types.cpp](src/emu/common/src/types.cpp) `translate_protection`，单一收口处）：
W^X 平台把 `prot_read_write_exec` 降级成 `PROT_READ|PROT_WRITE`（去掉 host 的 EXEC），
非 W^X 平台仍给真 RWX。所有 RWX chunk 统一受益,不只 trampoline。guest 仍通过模拟 CPU
「执行」这块内存,语义不变。
- 附带:[virtualmem.cpp](src/emu/common/src/virtualmem.cpp) `map_memory` 非 WASM 路径
  `mmap` 的 `fd` 从误写的 `0` 改 `-1`（musl/OHOS 比 glibc 严），失败时把 `MAP_FAILED`
  归一化成 `nullptr`,否则调用方只判 null 会漏检、继续往 `(char*)-1+off` 写。

### 教训

1. **W^X 不止卡 JIT,也卡任何 RWX 映射**:模拟器里「可执行」的 guest chunk 在 host 看来
   只是数据(解释器读字节、JIT 编进别的缓冲),host 侧根本不需要 PROT_EXEC。给 guest 内存
   申请 host RWX 在 W^X 平台一律失败。把 RWX 降级 RW 收口在 `translate_protection` 最干净。
2. **`mprotect` 失败要顺着传到清零写之前**:本案 commit 失败没拦住后面的 `std::fill`,才
   让一个 mprotect 的 -1 变成 SIGSEGV。映射类失败要么不往下走、要么 fail 得更早更明确。
3. **截断日志会把人带偏**:前一版 28 行日志缺寄存器/完整栈,addr2line 又因
   `.debug_ranges` 缺失把行号标到邻近 TU,差点定位成 mem-model chunk;拿到完整栈 + 寄存器
   (`x1=0x4000` 直接指向 trampoline)才一锤定音。要完整 crashlog 再下结论。

---

## 6.7 第五轮真机崩溃 — 点开游戏 SIGABRT:`ftell(null)`(缺着色器 + musl 严格)

承接 §6.6:装 ROM/起 app 都过了(进程活 12s、进了 RunPage),点开游戏后图形线程 abort。

### 错误:图形线程 `do_init` 加载着色器,文件没打开就 `ftell(null)` → musl abort

**现象** `SIGABRT`,`LastFatalMessage:__ftello: parameter is null`,图形线程。栈:

```
#02 ftello (musl)                              ← abort 源
#03 common::ro_std_file_stream::tell()
#04 common::ro_std_file_stream::size()
#05 drivers::ogl_shader_module::ogl_shader_module(path,...)
#07 ogl_graphics_driver::do_init()             ← 首次 draw_bitmap 时延迟初始化
#08 ogl_graphics_driver::draw_bitmap(...)
```

**两个叠加的根因**
1. **着色器文件没进 HAP**(功能性根因):着色器源在 `src/emu/drivers/resources/gles/*`,
   `build_hos_native.sh` 的 `stage_assets()` 会把它们拷进 `entry/src/main/resources/rawfile/
   assets/resources/`(扁平化 gles→resources,匹配代码里的相对路径 `resources//sprite_norm.vert`),
   再由 `Emulator.ets` 首次运行拷进沙箱。但**直接用 DevEco「运行」打的 HAP 不会跑这个 shell
   脚本的 staging 步骤**→`rawfile/assets/` 是空的→HAP 里没有着色器→`do_init` 第一个着色器
   就打不开。(本轮排查时 `rawfile/assets/` 目录根本不存在,坐实了这点。)
2. **`ro_std_file_stream` 对 null `FILE*` 不设防**(健壮性根因):`open_c_file` 失败后 `fi_`
   为 null,而 `tell()/size()/read()/seek()` 直接拿 `fi_` 调 `ftell/fread/...`。glibc 的
   `ftell(null)` 返回错误,**musl 的 `ftell(null)` 直接 abort 整个进程**——又一例 musl 比
   glibc 严。`ogl_shader_module` 构造里虽然 `if (!stream.valid()) LOG_ERROR(...)`,但只打日志
   **没 return**,径直走到 `stream.size()` → 触发 abort。

**修复(健壮性,代码层)**
- [buffer.h](src/emu/common/include/common/buffer.h) `ro_std_file_stream`:`tell/size/read/seek`
  全部加 `if (!fi_)` 防护(tell/size 返 0、read 返 0、seek 直接 return)。任何打开失败的只读
  文件流不再崩,优雅退化。
- [shader_ogl.cpp](src/emu/drivers/src/graphics/backend/ogl/shader_ogl.cpp)
  `ogl_shader_module` 构造:`!stream.valid()` 时 `LOG_ERROR` 后**直接 return**,不再拿空流
  去 `size()`/编译空着色器。

> 注意:代码层修复只是**让缺着色器不崩**;要真正出画面,HAP 必须带着色器——用
> `build_hos_native.sh`(它会 `stage_assets`)打包,或在 DevEco 打包前手动跑一次
> `stage_assets`,别直接用 DevEco「运行」按钮(它不跑 shell 脚本的 staging)。

### 教训

1. **musl 的 stdio 对 null 句柄是 abort,不是返错**:`ftell/fseek/fread(null)` 在 glibc 上
   宽容、在 musl(安卓/OHOS)上直接杀进程。所有「打开可能失败的文件」封装,方法体内必须判空,
   不能只提供一个 `valid()` 让调用方自觉。
2. **`valid()` 检查后要真的 `return`**:只 `LOG_ERROR` 不 return = 没检查。本案就差这一个
   return,把「缺文件」变成「崩溃」。
3. **DevEco「运行」≠ `build_hos_native.sh`**:shell 脚本里的 `stage_assets`(拷着色器/补丁/
   compat)是 IDE 构建流程之外的,直接 IDE 跑会漏掉运行期资源。资源类缺失要先确认 HAP 里到底
   有没有,别只在代码里找 bug。

## 6.8 第六轮 — 点开游戏黑屏(有 FPS):着色器仍没进 HAP(§6.7 的后半)

§6.7 修了「缺着色器不崩」,但治标:用 DevEco 直接打的 HAP 里**根本没有着色器**,所以不崩
之后变成**黑屏(但 FPS 正常显示)**。

### 判定:不是卡顿,是渲染没出来(着色器空程序)

`unzip -l entry-default-signed.hap` 确认包内**无 `rawfile/`、无任何 `.vert`/`.frag`、无
`defaultbank`**(只有 ArkUI 工程自己的 `resources/base/media/*` 图标)。运行时 `do_init`
8 个 `ogl_shader_module` 全部打开失败(§6.7 修复后优雅跳过,`shader` 句柄留 0)→
`sprite_program->create(...)` 等用空模块 link → 所有 program 无效 → 画位图用无效 program
→ **全黑**。FPS 正常是因为主循环/命令队列照跑,纯粹着色器没编译成功。

「有 FPS + 黑屏」的指纹:**渲染管线在转,但着色器/纹理/program 没就位**。先确认资源是否
真在包里(`unzip -l`),再看 shader 编译日志,别当成性能卡顿去查。

### 解法:让着色器进 HAP(打包问题,非代码 bug)

`build_hos_native.sh` 的 `stage_assets()` 从 `src/emu/drivers/resources/gles/*` 拷到
`entry/src/main/resources/rawfile/assets/resources/`(扁平化 gles→resources,匹配代码相对
路径 `resources//sprite_norm.vert`)。本轮已手动跑过 staging,`rawfile/assets/resources/`
现含 `do_init` 需要的全部 8 个着色器 + `defaultbank.{hsb,sf2}` + `upscale/` + `compat/`。
**用 `build_hos_native.sh` 打包(或打包前先跑 `stage_assets`),不要用 DevEco「运行」按钮。**

> 着色器进包只是第一步:`glCompileShader/glLinkProgram` 还得在真机 GLES 上成功。着色器是
> 标准 `#version 300 es` + `precision highp float`,鸿蒙 GLES 应认;若带上后仍黑屏,日志里
> 会有 `shader_ogl.cpp` 的 `glGetShaderInfoLog` 编译错误(那才是另一类问题)。

## 6.9 第七轮 — 装完设备(ROM)后 app 列表不刷新(OS 线程异步起,列表读太早)

§6.1 修的是装 **SIS** 后列表不刷新(`rescanApps`);这次是装完 **设备/ROM** 后列表空。
是不同场景、不同根因。

### 根因:`bootFirstDevice()` 只「启动」OS 线程,applist 还没扫完就 `getApps()`

装第一个设备的流程([Index.ets](src/emu/hos/entry/src/main/ets/pages/Index.ets) `onInstallRom`):
`installDevice()` → `bootFirstDevice()` → `refresh()`。而 native 的
[boot_first_installed_device](src/emu/hos/native/src/thread.cpp) 做的是
`bring_up_after_install()` + `stage_two()` + **`std::thread(os_thread)`** —— 起完线程立刻返回。
applist 服务端是 Symbian OS 在 `symsys->loop()` 里**异步**启动并扫注册表的,等 `os_thread`
真正跑起来要好些 wall-clock 迭代。所以 `bootFirstDevice()` 一返回就 `getApps()`,服务端还没
扫,`alserv->get_registerations()` 是空 → 列表空。

对照桌面 [main_window::on_new_device_added](src/emu/qt/src/mainwindow.cpp):它在
`startup()+set_device()` 后 `init_event.set(); init_event.wait();` **阻塞等 OS 线程重初始化
完成**再 `setup_app_list()`。但 HOS/WASM 的整个崩溃史(错误 14-16)就是「ArkUI/主线程不能
阻塞等 OS 线程」(`Atomics.wait`/`join`/`event.wait` 会死锁/ANR),所以不能照抄那个 wait。

### 修复(ArkUI 侧轮询,不阻塞):`refreshAppsWhenReady()`

新增 `refreshAppsWhenReady(maxWaitMs=8000, stepMs=300)`:`refresh()` 后若 `apps` 仍空,
就每 300ms `rescanApps()`+`refresh()` 重试,直到 app 出现或超时。设备是同步就绪的(立刻显示),
只有 app 需要等。两处接入:
- `onInstallRom`:装完第一个设备 `bootFirstDevice()` 后调它(显示「正在启动设备」),不再用
  会读到空列表的同步 `refresh()`;设备已在跑(非首装)时仍用同步 `refresh()`。
- `boot()`:从存储恢复的设备开机时 OS 线程也是异步起的,若「有设备但 app 空」同样轮询。

`rescanApps()`/`getApps()` 对 `alserv` 仍为 null 时是安全的(launcher 里 `rescan_apps` 会
`retrieve_servers()` 重解析、`get_apps` 判空返回空),所以轮询早期服务端没起来也不会崩。

### 教训

1. **「启动 OS 线程」≠「OS 已就绪」**:Symbian 服务端(applist)是在 guest 的 `loop()` 里
   异步 boot 的,起线程的调用一返回就读服务端状态必然过早。要么等就绪信号(桌面的 `init_event`),
   要么 UI 侧轮询——W^X/无阻塞平台只能选后者。
2. **同一症状两个根因**:装 SIS 不刷新(§6.1,服务端缓存→`rescanApps`)和装设备不刷新(本轮,
   服务端尚未起→轮询)看着一样,修法不同。先分清「服务端在不在 + 扫没扫」。

## 7. 已知遗留 / 待办

- [ ] **无声音**：null 音频现在是「自走时静音流」（不崩不卡，但没声）。后续接 Audio Kit
  的 `AudioRenderer`（PCM 回灌）出真实声音。
- [ ] **无网络**：null 蓝牙/internet。需要 libuv 的 OHOS 适配或换实现。
- [ ] **无摄像头/传感器/振动**：都走 null 后端。可接 Camera Kit / Sensor / Vibrator Kit。
- [ ] `launch_browser` no-op → 接 `startAbilityByType('browser', { uri })`。
- [ ] 设置页设备改名是占位（需要自定义输入对话框）。
- [ ] CPU 后端（§6.5 改为运行期探测：商店签名走 dyncom 解释器、开发签名走 dynarmic JIT）。
  store 构建用 dyncom 性能待观察；需在真机分别用商店签名与开发签名验证探测的两条分支。
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

**第三轮真机修复（§6.5，装 ROM 即崩 = 误选 Dynarmic JIT；改为运行期探测自动降级）**
| 文件 | 改动 |
|---|---|
| `src/emu/common/include/common/virtualmem.h`、`src/.../virtualmem.cpp` | 新增 `is_executable_memory_available()`（mmap+mprotect+执行探测，结果缓存）；`is_memory_wx_exclusive()` 把 OHOS 加入返回 true |
| `src/emu/system/src/epoc.cpp` | OHOS 单独分支：`is_executable_memory_available() ? dynarmic : dyncom`；include `common/virtualmem.h` |

**第四轮真机修复（§6.6，dispatcher trampoline RWX 被 W^X 拒绝）**
| 文件 | 改动 |
|---|---|
| `src/emu/common/src/types.cpp` | `translate_protection`：W^X 平台把 `prot_read_write_exec` 降级为 RW（去掉 host EXEC）；include `common/virtualmem.h` |
| `src/emu/common/src/virtualmem.cpp` | `map_memory` 非 WASM 分支 `mmap` fd `0→-1`，`MAP_FAILED` 归一化为 nullptr |

**第五轮真机修复（§6.7，点开游戏 `ftell(null)` abort）**
| 文件 | 改动 |
|---|---|
| `src/emu/common/include/common/buffer.h` | `ro_std_file_stream` 的 `tell/size/read/seek` 加 null `FILE*` 防护（musl `ftell(null)` 会 abort） |
| `src/emu/drivers/src/graphics/backend/ogl/shader_ogl.cpp` | `ogl_shader_module` 构造遇无效流 `LOG_ERROR` 后直接 return |
| （非代码）`build_hos_native.sh` `stage_assets` | 提醒：必须经此打包才会带着色器，别直接用 DevEco「运行」 |

**第七轮真机修复（§6.9，装完设备后 app 列表不刷新）**
| 文件 | 改动 |
|---|---|
| `src/emu/hos/entry/src/main/ets/pages/Index.ets` | 新增 `refreshAppsWhenReady()` 轮询（OS 线程异步起 applist）；`onInstallRom` 首装后、`boot()` 恢复设备后接入 |

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
