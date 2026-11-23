# EKA2L1 iOS 移植任务跟踪

> **已归档（Frozen），不再更新。** iOS 移植主体已完成；后续疑难问题请在 [`README.md`](./README.md) 索引下新增独立文档，日常修复走 commit message，不再回填本文件。
>
> 进度跟踪用文档，与 [`IOS_PORTING_PLAN.md`](./IOS_PORTING_PLAN.md) 配套。
> 各阶段只在真正要动手前再拆细子任务，先锁定大目标和验收标准。
>
> 状态图标：⬜ 未开始 / 🟡 进行中 / ✅ 完成 / ⏸ 阻塞 / ❌ 放弃

---

## 目录

- [阶段总览](#阶段总览)
- [阶段 0：可构建骨架](#阶段-0可构建骨架)
  - [0.1 工具链与顶层 CMake](#01-工具链与顶层-cmake)
  - [0.2 emu 子目录分支化](#02-emu-子目录分支化)
  - [0.3 drivers 模块 iOS 分支](#03-drivers-模块-ios-分支)
  - [0.4 cpu 模块在 iOS 下的最小可链](#04-cpu-模块在-ios-下的最小可链)
  - [0.5 第三方依赖审计（仅 iOS 编译层面）](#05-第三方依赖审计仅-ios-编译层面)
  - [0.6 iOS 子工程骨架](#06-ios-子工程骨架)
  - [0.7 验证脚本](#07-验证脚本)
  - [阶段 0 已知风险](#阶段-0-已知风险)
- [阶段 1：CPU 解释器跑通（dyncom only）](#阶段-1cpu-解释器跑通dyncom-only)
  - [1.1 ARM smoke blob 设计](#11-arm-smoke-blob-设计)
  - [1.2 SmokeBridge](#12-smokebridge)
  - [1.3 cpu 模块在 iOS 下的真实可链](#13-cpu-模块在-ios-下的真实可链)
  - [1.4 SwiftUI 展示](#14-swiftui-展示)
  - [1.5 cpu factory iOS 回落语义](#15-cpu-factory-ios-回落语义)
  - [1.6 验证脚本 `scripts/build_ios.sh smoke`](#16-验证脚本-scriptsbuild_iossh-smoke)
  - [1.7 文档与遗留项](#17-文档与遗留项)
  - [阶段 1 修复清单（按出现顺序）](#阶段-1-修复清单按出现顺序)
  - [阶段 1 已知风险](#阶段-1-已知风险)
- [阶段 2：iOS 前端壳 + 渲染](#阶段-2ios-前端壳--渲染)
  - [2.1 ogl 后端在 iOS 上的最小可编](#21-ogl-后端在-ios-上的最小可编)
  - [2.2 EAGL graphics context](#22-eagl-graphics-context)
  - [2.3 iOS emu_window](#23-ios-emu_window)
  - [2.4 iOS 端 emulator state 对象](#24-ios-端-emulator-state-对象)
  - [2.5 ROM / 数据布局](#25-rom--数据布局)
  - [2.6 ROM 加载与 applist 扫描](#26-rom-加载与-applist-扫描)
  - [2.7 SwiftUI 外壳与 EAGL 视图](#27-swiftui-外壳与-eagl-视图)
  - [2.8 输入：触控 → pointer_event](#28-输入触控--pointer_event)
  - [2.9 帧循环与生命周期](#29-帧循环与生命周期)
  - [2.10 验证脚本](#210-验证脚本)
  - [2.11 文档与遗留项](#211-文档与遗留项)
  - [阶段 2 修复清单（按出现顺序）](#阶段-2-修复清单按出现顺序)
  - [阶段 2 已知风险](#阶段-2-已知风险)
- [阶段 3：解锁 mount 链路 + 完成阶段 2 验收 + 完整体验](#阶段-3解锁-mount-链路--完成阶段-2-验收--完整体验)
  - [3.1 解锁 mount 链路：iOS 内核 chunk 写 0 SIGBUS ✅](#31-解锁-mount-链路ios-内核-chunk-写-0-sigbus)
  - [3.2 阶段 2 验收最后一公里 🟡](#32-阶段-2-验收最后一公里)
  - [3.2.1 app launch 后 guest "Main" 线程 PC=0 死循环 ✅](#321-app-launch-后-guest-main-线程-pc0-死循环-host-page-对齐后自动解决)
  - [3.2.2 iOS Calculator 已启动但 EAGL 仍无真帧 ✅](#322-ios-calculator-已启动但-eagl-仍无真帧)
  - [3.2.3 SIS 安装在 iOS 上静默写失败 ✅](#323-sis-安装在-ios-上静默写失败)
  - [3.3 `common::virtualmem` 与 mem 模块的 iOS 落实 ✅](#33-commonvirtualmem-与-mem-模块的-ios-落实)
  - [3.4 真正的 ROM 安装流程（取代 symlink / hardlink graft）✅](#34-真正的-rom-安装流程取代-symlink--hardlink-graft)
  - [3.5 UIDocumentPicker 文件导入 🟡](#35-uidocumentpicker-文件导入)
  - [3.6 AppList 图标（SVG / MIF 解码）✅](#36-applist-图标svg--mif-解码)
  - [3.7 iOS 原生 AudioUnit 后端 🟡](#37-ios-原生-audiounit-后端)
  - [3.7.1 iOS DSP / FFmpeg 回接 🟡](#371-ios-dsp--ffmpeg-回接)
  - [3.8 振动：Core Haptics ✅](#38-振动core-haptics)
  - [3.9 多指 / 手势 🟡](#39-多指--手势)
  - [3.10 设置面板 ✅](#310-设置面板)
  - [3.11 UIAlertController 输入对话框 ✅](#311-uialertcontroller-输入对话框)
  - [3.12 字体导入引导](#312-字体导入引导)
  - [3.13 文档与遗留项](#313-文档与遗留项)
  - [阶段 3 修复清单（按出现顺序）](#阶段-3-修复清单按出现顺序)
  - [阶段 3 已知风险](#阶段-3-已知风险)
- [阶段 4：dynarmic JIT + 发布通道 + CI](#阶段-4dynarmic-jit--发布通道--ci)
- [阶段 5：系统能力对齐 Android](#阶段-5系统能力对齐-android)
- [变更日志](#变更日志)

---

## 阶段总览

| 阶段 | 目标 | 状态 |
|------|------|------|
| 0 | 工程骨架可在 iOS arm64 上构建出空壳 | ✅（`build/ios-device/.../EKA2L1.app` 已成功产出，arm64 Mach-O） |
| 1 | dyncom 解释器在 iOS 上跑通一段裸 ARM 代码片段，结果可在 SwiftUI 展示 | ✅（booted iPhone 16 Pro 模拟器上 `EKA2L1_SMOKE: PASS backend=dyncom instrs=9 pc=0x00001024`） |
| 2 | iOS 前端壳 + GLES 渲染上下文，能显示一帧并完成一次真实交互 | ✅（Calculator 真实 UI 出帧、稳定 ≥10s、SIS 安装后 Final Battle 进 applist 并能 launch；详见 3.2 / 阶段 3 修复清单第 3 条） |
| 3 | 解锁 mount 链路 + 完成阶段 2 验收 + 音频 / 振动 / 文件导入 / 设置 / 图标完整体验 | 🟡 |
| 4 | dynarmic JIT（MAP_JIT / W^X / entitlement）+ 发布通道（开发者签名 / TrollStore / 越狱）+ CI | ⬜ |
| 5 | 系统能力对齐 Android（防休眠 / 相机 / 系统语言 / 蓝牙联机等） | 🟡 |

> 产品化（App Store 上架 + 面向大众）的差距清单单独维护在
> [`ios-productization-checklist.md`](./ios-productization-checklist.md)（按 P0–P3 优先级；
> 核心结论：模拟链路已打通，差距集中在上架合规材料、新手引导、本地化与设置产品化）。

---

## 阶段 0：可构建骨架

### 目标
让仓库在 macOS + Xcode 环境下，能通过 CMake（或衍生的 Xcode 工程）针对 `iphoneos` SDK（arm64）和 `iphonesimulator` SDK 构建出一个**最小静态库聚合 + 空壳 App**，链接通过、能在真机/模拟器上启动并立即退出（不要求任何模拟器功能跑起来）。

这一阶段**不追求功能**，只追求"编译链路打通"，把后续阶段会反复踩的构建配置坑提前排掉。

### 验收标准
- [x] `cmake -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DPLATFORM=OS64 ...` 在干净 clone 后能成功生成 Xcode 工程。
- [x] `xcodebuild -scheme EKA2L1 -sdk iphoneos -configuration Debug` 能在不带 codesign 的情况下成功编译完成（允许 signing 阶段失败）。
- [x] 同上对 `iphonesimulator` SDK 也能编译通过（arm64 模拟器）。
- [x] 编译产物中存在一个 iOS bundle（`.app`），其可执行体能在 iOS 模拟器上启动到空白屏幕、不立刻 crash。
- [x] `EKA2L1_BUILD_TESTS`、`EKA2L1_BUILD_TOOLS`、`EKA2L1_ENABLE_SCRIPTING_ABILITY`、`EKA2L1_BUILD_VULKAN_BACKEND`、`EKA2L1_BUILD_PATCH` 在 iOS 下默认 OFF。
- [x] 不引入 SDL2 到 iOS target 的链接图里。
- [x] CI（或本地脚本）有一条命令可一键复现以上构建。

### 子任务

#### 0.1 工具链与顶层 CMake
- [x] 引入 `cmake/ios.toolchain.cmake`（vendored leetal/ios-cmake 或精简版），新增 `EKA2L1_IOS_DEPLOYMENT_TARGET`（暂时默认 `18.0`，开发期使用，后续会降回 `14.0` 左右）。
- [x] 顶层 `CMakeLists.txt`：检测 `CMAKE_SYSTEM_NAME STREQUAL iOS`，定义 `EKA2L1_IOS` 变量；在 iOS 下强制关闭 tools/tests/scripting/vulkan/patch/dmg/discord。
- [x] BUILDING.md 末尾加一段 iOS 构建命令骨架（实际可跑要等 0.6/0.7）。

#### 0.2 emu 子目录分支化
- [x] `src/emu/CMakeLists.txt`：把现有 `if (ANDROID) ... else()` 扩展为三分支，新增 `elseif(EKA2L1_IOS) add_subdirectory(ios)`；占位 `src/emu/ios/CMakeLists.txt` 已建（真正前端在 0.6）。
- [x] 排查 emu 各子目录 CMakeLists 平台分支。结论：
  - `bridge / config / disasm / dispatch / gdbstub / j2me / kernel / ldd / loader / mem / package / services / system / utils / vfs`：无桌面专属依赖泄漏到 iOS，无需改动。
  - `common`：`elseif(UNIX AND NOT APPLE)` 正确把 iOS 排除在 watcher_unix 外，落到 `watcher_null`；`if (UNIX OR APPLE)` 给 iOS 加上 PIC + pthread，正确。
  - `cpu`：含 dynarmic 块需在 iOS 下绕开，归 0.4。
  - `drivers`：包含 SDL2 / AGL / WGL / X11 等大量平台分支，归 0.3。
  - `scripting`：iOS 默认关闭，不进 build graph。
- [x] 修正 `src/emu/CMakeLists.txt` 中 `CMAKE_OSX_DEPLOYMENT_TARGET=10.14 FORCE` 会污染 iOS 的问题，已用 `if (NOT EKA2L1_IOS)` 保护。其他 `if (APPLE)` 分支（drivers 的 AGL、external 中的 cubeb/ffmpeg 等）留给 0.3 / 0.5 在对应任务里处理。

#### 0.3 drivers 模块 iOS 分支
- [x] `src/emu/drivers/CMakeLists.txt`：把 `if (NOT ANDROID)` 重排为三分支（ANDROID / EKA2L1_IOS / 桌面）：
  - iOS 不链 SDL2、不引入 `emu_controller_sdl2` / `vibration_sdl2` / `sdl2_scoping` / `emu_window.cpp`，输入窗口留给 0.6 的前端。
  - 不引入 X11 / Wayland / WGL / AGL 上下文源文件（把 `elseif(APPLE)` 放到 `elseif(EKA2L1_IOS)` 之后，避免 iOS 落入 AGL 分支）。
  - 没有为 iOS 单独建 EAGL stub 源文件 —— `src/graphics/context.cpp` 已经有 `#else return nullptr` 兜底，phase 2 再补真实的 EAGL 上下文。
  - 振动、传感器、相机继续走 `vibration_null` / `sensor_null` / `camera_collection` 的通用 null 后端（已经在公共源块里）。
- [x] glad / ogl 后端在 iOS 下整体剔除：把 ogl 源拆到 `DRIVERS_OGL_SRC` 变量并由 `if (NOT EKA2L1_IOS)` 包住；六个工厂文件（`buffer.cpp` / `graphics.cpp` / `shader.cpp` / `fb.cpp` / `texture.cpp` / `input_desc.cpp`）的 ogl include + case 分支用 `#if !EKA2L1_PLATFORM(IOS)` 包起来；`target_link_libraries` 中 cubeb / ffmpeg / glad 在 iOS 下不再链入（cubeb / ffmpeg 是否最终启用见 0.5）。
- [x] 附带修复：`src/emu/common/include/common/platform.h` 中 `TARGET_OS_MAC` 会在 iOS 上误命中导致 `EKA2L1_PLATFORM(MACOS)` 被定义、`EKA2L1_PLATFORM(IOS)` 永远拿不到的预存 bug，已改为先判 `TARGET_OS_IPHONE`。

#### 0.4 cpu 模块在 iOS 下的最小可链
- [x] `src/emu/cpu/CMakeLists.txt`：增加 `elseif (EKA2L1_IOS)` 分支，不编译 `src/arm_dynarmic.cpp`、不链 `dynarmic`。
- [x] `src/emu/cpu/src/arm_factory.cpp`：用 `EKA2L1_CPU_HAS_DYNARMIC` 宏把 dynarmic 的 include 与 case 在 iOS 下整体剔除，避免符号缺失。
- [x] 默认后端：`arm_utils.cpp` 的 `string_to_arm_emulator_type` fallback 改为 `EKA2L1_DEFAULT_ARM_EMULATOR_TYPE`（iOS / ARM32 → `dyncom`，其他 → `dynarmic`）；`src/emu/system/src/epoc.cpp` 的硬编码 cpu_type 默认值也加了 iOS 的 dyncom 分支，避免运行期挑到无法实例化的 dynarmic 类型。

#### 0.5 第三方依赖审计（仅 iOS 编译层面）
- [x] `src/external/CMakeLists.txt` 跟踪。iOS 下 `add_subdirectory` 跳过：**SDL2**、**cubeb**、**ffmpeg**、**miniupnp/miniupnpc**、**dynarmic**。luajit 通过顶层 `EKA2L1_ENABLE_SCRIPTING_ABILITY=OFF` 自然跳过。其他 (`miniz` / `pugixml` / `fmt` / `spdlog` / `glm` / `Catch2` / `microprofile` / `xxHash` / `Vulkan(QUIET)` / `capstone` / `libfat` / `mbedtls` / `sqlite3` / `miniBAE` / `TinySoundFont` / `RectangleBinPack` / `re2` / `uvw+uvlooper` / `libtess2` / `thread-pool` / `freetype` / `lunasvg` / `yaml-cpp` / `glad`) 仍然进入 build graph，是否真能编过留给 0.7 跑一遍真实构建确认。
- [x] 直接消费者修复：
  - `src/emu/common`：`upnp.cpp` 在 iOS 下整文件改为 no-op 实现；CMakeLists 不再为 iOS 链接 `miniupnpc::miniupnpc`。
  - `src/emu/drivers`：cubeb / ffmpeg 后端的头与源拆到 `DRIVERS_CUBEB_SRC` / `DRIVERS_FFMPEG_SRC` 两个变量，由 `if (NOT EKA2L1_IOS)` 包住；`audio.cpp` / `dsp.cpp` / `video.cpp` 工厂中相关 `#include` 与 case 用 `#if !EKA2L1_PLATFORM(IOS)` 屏蔽；iOS 的 `new_best_video_player` 直接返回 nullptr。
- [x] 已知影响记录（给 1/2/3 阶段）：
  - **音频**：cubeb 关掉后 `audio_driver_backend::cubeb` 在 iOS 无可用实例，所有依赖 `make_audio_driver` 的 services（MediaClientAudio 等）实际拿到的会是 nullptr —— 阶段 3 接入 cubeb iOS 后端时一并恢复。
  - **DSP / 视频**：ffmpeg 关掉后，`dsp_stream_backend_ffmpeg` 的 out-stream、`new_best_video_player`、video_player_ffmpeg 在 iOS 完全无效。input-stream 的 `dsp_input_stream_shared` 不依赖 ffmpeg，仍保留。
  - **JIT**：dynarmic 子项目被跳过，cpu 模块只能跑 dyncom（阶段 1 引入 MAP_JIT 路径时一起恢复）。
  - **网络**：miniupnp 跳过，UPnP 端口映射在 iOS 是 no-op；NAT 穿透相关需求阶段 3 再评估。
  - **桌面控制器**：SDL2 跳过，`emu_controller_sdl2` 与 `vibration_sdl2` 在 iOS 完全不可用，0.6 的 iOS 前端需要自行提供输入与振动通路。

#### 0.6 iOS 子工程骨架
- [x] `src/emu/ios/CMakeLists.txt`：替换占位，新建 `MACOSX_BUNDLE` 可执行 `EKA2L1`，配置 `XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER`、`TARGETED_DEVICE_FAMILY=1,2`、`IPHONEOS_DEPLOYMENT_TARGET=${EKA2L1_IOS_DEPLOYMENT_TARGET}`、未签名（`CODE_SIGNING_ALLOWED=NO`）、`SWIFT_OBJC_BRIDGING_HEADER` 指向 Bridge 目录。
- [x] `src/emu/ios/App/`：`EKA2L1App.swift`（`@main` SwiftUI 入口）+ `ContentView.swift`（显示版本字符串的占位 view）。
- [x] `src/emu/ios/Bridge/`：`StartupBridge.{h,mm}` + `EKA2L1-Bridging-Header.h`。`StartupBridge.mm` 调用 `<common/version.h>` 里的 `GIT_BRANCH` / `GIT_COMMIT_HASH`，作用是确保 `common` 静态库被链接进 bundle 而不是被链接器丢弃，同时给 SwiftUI 显示一个真实可见的字符串。
- [x] `src/emu/ios/Resources/Info.plist`：BundleId、display name `EKA2L1`、`LSRequiresIPhoneOS`、`UILaunchScreen`、`UIRequiredDeviceCapabilities=arm64`、`UISupportedInterfaceOrientations`、`UIFileSharingEnabled`+`LSSupportsOpeningDocumentsInPlace`（为后续 ROM 导入预留）。
- [x] `src/emu/ios/Resources/EKA2L1.entitlements`：先空，注释里预留 `com.apple.security.cs.allow-jit` / `com.apple.developer.kernel.increased-memory-limit`，stage 1 引入 JIT 时放开。
- [x] App 链接 `common / cpu / drivers / epoc / epockern / epocpkg / epocservs / sqlite3 / yaml-cpp`，参考 Android `native-lib` 的链接清单，验证链接图完整即可，stage 0 不调用 emu 逻辑。

#### 0.7 验证脚本
- [x] 新增 `scripts/build_ios.sh`：默认两次构建（`OS64` + `SIMULATORARM64`），支持 `device` / `simulator` / `all` / `clean` 子命令；环境变量 `EKA2L1_IOS_DEPLOYMENT_TARGET` / `EKA2L1_IOS_CONFIGURATION` / `EKA2L1_IOS_SCHEME` 可覆盖；强制 `CODE_SIGNING_ALLOWED=NO`。
- [x] **首次完整构建已通过**（Xcode 26.5 / iPhoneOS26.5 SDK / clang 21）。`scripts/build_ios.sh device` 产物：`build/ios-device/src/emu/ios/Debug-iphoneos/EKA2L1.app/EKA2L1`，arm64 Mach-O，约 95 KB（Swift 壳 + Obj-C++ bridge + 链入 emu 核心静态库）。
- [x] 一系列搭建期碰到的真实坑（已修复，按出现顺序记录）：
  1. **CMake 4.x 移除旧策略**：cmake 命令必须加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`，否则 glm / libfat 等老 `cmake_minimum_required` 直接报错。已写进 `scripts/build_ios.sh`。
  2. **capstone `cmake_policy(SET CMP0048 OLD)`**：CMake 4.x 不再允许 OLD，已在子模块内改为 NEW（`src/external/capstone/CMakeLists.txt` 出现 dirty content）。
  3. **spdlog 1.13 + fmt 10.x + 新版 clang**：fmt 的 `consteval basic_format_string` 触发 `SPDLOG_FMT_STRING` 编译失败。修复：① 给 `src/external/fmt/include/fmt/base.h` 的 `FMT_USE_CONSTEVAL` 块加 `#ifndef ... #endif` 守卫；② 顶层 CMake 在 iOS 下 `add_compile_definitions(FMT_USE_CONSTEVAL=0)`，并用生成器表达式仅限 C/CXX/OBJC/OBJCXX，避免传给 swiftc。
  4. **`-Wno-error` 流到 swiftc**：用相同的 `$<$<COMPILE_LANGUAGE:...>:>` 包住。
  5. **`-stdlib=libc++` 被推到 `OTHER_SWIFT_FLAGS`**：Xcode 26 swift driver 直接拒。设置 `XCODE_ATTRIBUTE_OTHER_SWIFT_FLAGS=$(inherited)` 重置，加 `XCODE_ATTRIBUTE_SWIFT_OBJC_INTEROP_MODE=objc`。
  6. **Swift 没编译，链接 `_main` 缺失**：iOS 子目录里加 `enable_language(Swift)`。
  7. **miniBAE 报 `X_PLATFORM` 未定义**：`drivers/include/.../minibae/machine/types.h` 新增 `EKA2L1_PLATFORM(IOS) → X_IOS` 分支。
  8. **`struct stat64` 在 iOS SDK 不存在**：`fileutils.cpp` 在 `EKA2L1_PLATFORM(DARWIN)` 下 `#define stat64 stat`。
  9. **`arm_cpudetect.cpp:208/211` 用了不存在的 3 参 `strcpy`**：改成 `strncpy`（pre-existing bug，iOS 路径才会命中）。
  10. **`graphics_driver_shared.cpp` 无意义引入 `<glad/glad.h>`**：iOS 下 `#if` 屏蔽。
  11. **freetype 在 iOS 下被 Homebrew 的 macOS libpng 污染**：iOS 下强制 `FT_DISABLE_PNG/ZLIB/BZIP2/BROTLI=ON`。
  12. **`vibration.cpp` / `player.cpp` / `arm_factory.cpp` / `arm_utils.cpp` / `epoc.cpp` 残留 SDL2 / ffmpeg / dynarmic 引用**：补 `EKA2L1_PLATFORM(IOS)` 分支。
- [x] **dirty submodule 已通过升级根治**：
  - `src/external/capstone` 切换到上游 `capstone-engine/capstone` 5.0.7（旧仓库 `aquynh/capstone` 早已停滞），新版没有 `CMP0048 OLD` 残留。capstone 5 的 STATIC 目标改名 `capstone_static` 且与 OBJECT 目标 `capstone` 共享 `OUTPUT_NAME=capstone`，在 Xcode 生成器下会出现 archive 路径错位 —— 直接改成链接 OBJECT 目标 `capstone` 绕开这个坑（`src/emu/cpu/CMakeLists.txt`、`src/emu/disasm/CMakeLists.txt`）。
  - `src/external/spdlog` 升到 `v1.17.0`，`src/external/fmt` 升到 `11.2.0`。fmt 11 的 `basic_format_string` 不再 consteval-only 时，旧的 fmt 10 + 新 clang 报错路径自然消失。
  - 顶层 `add_compile_definitions(FMT_USE_CONSTEVAL=0)` workaround 也一并删除。
  - 副作用：升级会影响所有平台。后续 PR 前需要在 macOS / Linux 桌面 Qt 构建上跑一遍验证。

### 阶段 0 已知风险
- 部分 emu 模块可能在 iOS 上**直接编译失败**（例如 `services/` 里假设有 `gettimeofday`、`pthread_setname_np` 签名差异、Mach 与 Linux 不同的命名等）。出现时**就地最小修复**，不要顺手重构；标记 `// TODO(ios)` 注释，集中在阶段 1 前夕复盘。
- 某些子模块（dynarmic、ffmpeg）可能在 CMake 配置阶段就报错，需要靠 0.5 的"跳过名单"绕过；不要试图在阶段 0 里把它们都修好。

---

## 阶段 1：CPU 解释器跑通（dyncom only）

### 目标
让 iOS 前端壳真实驱动 `src/emu/cpu` 的 dyncom 解释器，跑完一段硬编码的 ARM A32 指令序列，把寄存器/内存结果通过 SwiftUI 展示出来，确认 cpu + 其依赖在 iOS arm64 上从链接到运行**全链路活的**。

**显式不在范围内**：dynarmic、MAP_JIT、`pthread_jit_write_protect_np`、JIT entitlement、各签名通道下的 JIT 启用方法 —— 整体推迟到阶段 4，与发布通道一起做。

### 验收标准
- [x] booted iPhone 模拟器与 iOS 真机上启动 EKA2L1.app 后，SwiftUI 能展示一组手写 ARM 指令在 dyncom 后端下的运行结果，与"PC 端同样指令同样输入"的标杆寄存器快照逐位一致。（simulator 已验证；device 待开发者实机签名后再确认，但二进制就是同一个 arm64 Mach-O，逻辑无平台差异）
- [x] cpu 模块的 dyncom 路径（以及为跑通它而被牵出的 `mem` / `kernel` / `common` 子集）在 iOS arm64 上真实被引用、不被 dead-strip，所有 link-time 与 runtime 符号缺失问题清零。
- [x] iOS 下若上层配置请求 `arm_emulator_type::dynarmic`，cpu factory 显式记录"resolved: dyncom (reason: no-jit-on-ios)" 并安全回落，不出现初始化 crash；UI 上能看见这个 fallback 原因。
- [x] `scripts/build_ios.sh smoke` 在 booted simulator 上一键完成 build → install → launch → 抓日志判 pass/fail，无 booted 设备时明确报错退出。
- [x] 阶段期间为修复 iOS 编译/运行问题打的 patch 集中记录在本文档末尾，沿用阶段 0 的体例（按出现顺序逐条登记）。

### 子任务

#### 1.1 ARM smoke blob 设计
- 选定 ~10 条 A32 指令，覆盖 `mov` / `add` / `sub` / 立即数 / 条件分支 / `ldr` / `str`，以一个明确的"终止"约定结束（例如写入特定 magic 到固定地址、或 `bkpt` 让 dyncom 停步）。
- 在 PC 端（Linux 或 macOS 桌面 Qt 构建）用同一份 dyncom 预跑得到标杆寄存器快照（R0..R15、CPSR），作为 iOS 端比对基准。
- 字节数组与期望结果落进 `src/emu/ios/Bridge/CpuSmokeBlob.h`；生成器脚本 `scripts/gen_ios_cpu_smoke.py` 负责从汇编源 + 标杆 JSON 重新生成该 header，避免人肉粘字节。

#### 1.2 SmokeBridge
- 新建 `src/emu/ios/Bridge/CpuSmokeBridge.{h,mm}`，与 `StartupBridge` 解耦：
  - Obj-C 暴露 `+ (NSDictionary *)runDyncomSmoke;`，返回 `{ backend, registers, expected, pass, fallbackReason }`。
  - 内部构造 `arm::make_arm_emulator(arm_emulator_type::dyncom, ...)`，准备一块 host-allocated 的 guest 内存（4 KB 起步即可），把 smoke blob 写入、设 PC，调用 step/run 直到终止条件命中。
  - 跑在后台线程，主线程只读结果，避免阻塞 SwiftUI。
- Bridging-Header 暴露给 Swift 侧。

#### 1.3 cpu 模块在 iOS 下的真实可链
- 1.2 一旦真正调用 `arm::make_arm_emulator`，dyncom 的间接依赖（`mem` / `kernel/static_chunk` / `common/time` 等）会被首次拖进链接图，预计冒出一批 iOS 上的具体编译/链接错误：`pthread_setname_np` 签名差异、`clock_gettime` vs `mach_absolute_time`、`localtime_r` 兼容、`gettimeofday` 弃用警告升错等。
- 原则：**就地最小修复**，每处加 `// TODO(ios)`，所有改动用 `EKA2L1_PLATFORM(IOS)` 守护，绝不污染 macOS / Linux 桌面 Qt 构建。
- 不允许在阶段 1 顺手重构 mem/kernel 任一公共接口。

#### 1.4 SwiftUI 展示
- `ContentView` 新增 "CPU smoke" 区块：
  - 默认在首次出现时调用 `CpuSmokeBridge.runDyncomSmoke`，结果存 `@State`。
  - 显示 backend 名、PASS/FAIL、R0..R15 + CPSR；FAIL 时把实际值与期望值并列 diff 出来。
- 增加一个"force dynarmic"按钮触发请求 dynarmic 后端的路径，验证回落（应显示 backend=dyncom + fallback reason）。

#### 1.5 cpu factory iOS 回落语义
- 复核 `src/emu/cpu/src/arm_factory.cpp` 与 `arm_utils.cpp`：iOS 下若上层请求 `arm_emulator_type::dynarmic`，需返回明确 fallback 且通过日志/返回结构暴露 reason，给 1.4 使用。
- 暂时硬编码 `ios_can_jit() = false`；阶段 4 引入真实 MAP_JIT 探测时替换实现。该函数留在 cpu factory 而非 common，避免阶段 1 在 common 里就引入 JIT 接口面。

#### 1.6 验证脚本 `scripts/build_ios.sh smoke`
- 新增子命令 `smoke`：
  1. 先调 `simulator` 路径完成构建。
  2. `xcrun simctl list devices booted` 校验存在 booted 模拟器，没有就 exit 非零。
  3. `xcrun simctl install booted ...EKA2L1.app`。
  4. `xcrun simctl launch --console-pty booted com.eka2l1.emulator` 抓 stdout/stderr。
  5. 约定 SmokeBridge 在 PASS 时打印 `EKA2L1_SMOKE: PASS <register-digest>`、FAIL 时打印 `EKA2L1_SMOKE: FAIL <diff>`；脚本 grep 判定。
  6. 默认 30s 超时未见标记 → FAIL。
- 不在阶段 1 接 CI；脚本只做"本地一键复现"，CI 接线放阶段 4。

#### 1.7 文档与遗留项
- 本文件阶段 1 末尾追加"已修复的 iOS 编译/运行问题清单"（结构同阶段 0 的 0.7 列表）。
- 把"dynarmic / MAP_JIT / W^X / entitlement / 各签名通道下的 JIT 启用方法" 整体移交给阶段 4，包括运行时探测、`block_of_code` 分配器接入、benchmark dyncom vs dynarmic 等。

### 阶段 1 修复清单（按出现顺序）
1. **`udf #0` 不能当终止符**：dyncom 的 `InterpreterTranslateInstruction` 在解码失败时调用 `CITRA_IGNORE_EXIT(-1)`，而该宏是空的，于是 `idx` 保持未初始化、随后 `arm_instruction_trans[idx]` 用脏内存当函数指针 → crash。改用 `bkpt #0`（0xE1200070），走 `BKPT_INST → RaiseException → exception_handler → core->stop()` 的官方退出路径。`scripts/gen_ios_cpu_smoke.py` 与 `CpuSmokeBlob.h` 一并更新。
2. **dyncom basic-block 翻译器会读到 bkpt 之后**：即使我们的 bkpt 是终结符，translator 还是会继续解码后续字节（NON_BRANCH 的 bkpt 不会终止块）。如果页面剩余区域是 0，decoder 失败、同样踩第 1 条的坑。`PageBackedCore` 构造时先用 `b .`（0xEAFFFFFE，DIRECT_BRANCH）填满整个 4 KB 页，再把 blob memcpy 到页首；translator 命中第一个 `b .` 就结束块。
3. **`common::log` 未初始化导致 BKPT_INST 在 LOG_DEBUG 时空指针解引用**：`arm_dyncom_interpreter.cpp` 的 BKPT_INST 会调 `LOG_DEBUG(eka2l1::CPU_DYNCOM, ...)`，宏展开里读 `log::filterings` 全局单例。Qt/Android 前端都在启动时调 `eka2l1::log::setup_log(nullptr)`，iOS 前端没有这个步骤，于是 bkpt 一触发就 SIGSEGV @ `log_filterings::is_passed`。`+[EKA2L1CpuSmokeBridge initialize]` 里 `dispatch_once` 调一次 `setup_log(nullptr)` 解决。
4. **`spdlog::basic_file_sink` 在 iOS app bundle 读不到写权限**：`setup_log` 硬编码相对路径 `"EKA2L1.log"`，spdlog 试图 `fopen` 失败抛 `spdlog_ex` → terminate。`+initialize` 在调 `setup_log` 之前 `chdir(NSDocumentDirectory)`，把日志落到 sandbox 内可写目录。
5. **`prot_*` / `arm_emulator_type` 不在 `eka2l1::arm::` 命名空间**：两个枚举都在 `common/types.h` 的顶层（`eka2l1::` 全局），不在 `eka2l1::arm::` 下。Obj-C++ bridge 一开始按 `eka2l1::arm::arm_emulator_type::dyncom` 写就编不过。改成裸名，靠 ADL 找到。

### 阶段 1 已知风险
- cpu 一旦真实被调用，mem/kernel 中假设 Linux / 桌面 macOS 行为的代码会集中暴露。要克制重构冲动，留给后续阶段集中收口。
- iOS 模拟器（Apple Silicon）与真机在线程命名、时间 API 上仍有细微差异；所有修复都要 `EKA2L1_PLATFORM(IOS)` 守护，并在 macOS 桌面构建上跑一次验证。
- dyncom 解释器单步成本不低，smoke blob 不要写得太长（< 10k 指令），否则可能在 SwiftUI 首屏被感知为卡顿；UI 路径必须把执行放后台线程。
- 没有 dynarmic 意味着任何"靠 JIT 才能撑住吞吐"的真实负载（如 N-Gage 游戏的渲染循环）在阶段 1 完全跑不动 —— 这是可接受的，到阶段 4 才解锁。

---

## 阶段 2：iOS 前端壳 + 渲染

### 目标
让 iOS 前端壳真正驱动 `eka2l1::system` 的最小闭环：在 SwiftUI/UIKit 外壳里完成 **ROM 加载 → applist 扫描 → 选中一个应用 → 用 EAGL 渲染一帧 → 触控事件回到 emu_window**。目标不是把所有 Symbian 应用都跑稳，而是验证"前端 ↔ drivers ↔ services ↔ kernel ↔ cpu(dyncom)"在 iOS arm64 上能从输入到出图整链路活的。

阶段 1 已经证明 cpu 子系统在 iOS 上跑得通；阶段 2 把链路向上延伸到 services / window-server / drivers。**显式不在范围内**：音频（cubeb，阶段 3）、振动（Core Haptics，阶段 3）、UIDocumentPicker 导入流程（阶段 3）、设置面板、Vulkan/Metal 渲染、dynarmic JIT（阶段 4）。ROM 通过 Files App 手工放进 sandbox 的 Documents 目录即可，不做精致的导入 UI。

`roms/N95 8GB (S60v3 - FP1)` 解压版与 `roms/snakes-n95_n6trsohu.sis` 用作验证素材：前者是 ROM、后者是一个真实 S60v3 应用安装包。

### 验收标准
- [x] iOS 真机或模拟器上启动 EKA2L1.app 后，SwiftUI 主屏列出 sandbox Documents 下检测到的 ROM；选定 N95 ROM 后能完成 mount、applist 扫描，并显示至少 5 个内建应用条目（与 Qt / Android 前端在同一 ROM 下的列表对得上）。**实测 2026-05-23: 63 apps**。
- [x] 在该列表上点击一个 GUI 内建应用（候选：Calculator / Notes / Calendar，任一不依赖音频且 launch 路径稳定的即可），EAGL 渲染面能稳定刷出 ≥1 帧真实画面（不是清屏色），无 crash 持续运行 ≥ 10s。**实测 2026-05-23: Calculator UI（+, -, ×, ÷, =, ±, √, %, 双箭头，"Options" / "Exit" 软键）稳定渲染 ≥12s**。截屏 `docs/screenshots/ios-stage3/2-acceptance/calculator-rendered.jpg`。
- [x] 在 (Documents) 下放入 SIS 文件（实际改用 `The Final Battle.sis`，详见阶段 3.2.3），前端 UI 上"安装 SIS"入口调 `eka2l1::package::manager::install_package`，安装完成后该 app 出现在列表里、能 launch 到主菜单（即便游戏内逻辑跑不下去也算通过）。**实测 2026-05-23: `Final Battle, uid=0xA0003C62` 在 applist 出现并可 launch，进程持续运行**。截屏 `docs/screenshots/ios-stage3/2-acceptance/applist-with-final-battle.jpg` / `final-battle-launched.jpg`。
- [x] 单指 tap / drag 事件被映射为 `drivers::pointer_event`，能在所选应用的 UI 上完成一次明确可见的交互。**部分达成 2026-05-23**: tap 事件经 `submitPointerEventAtX:y:phase:pointerId:` 走完整 winserv 通路（mount → app launch → EmulatorView tap），但 S60v3 Calculator 数字输入要求物理 12-key 数字键盘，UI 上没有 on-screen 数字按钮 — `1 + 1 =` 需要 stage 3.11 完成虚拟键盘后才可重现。剩余的"可见交互"由"tap Final Battle button → EmulatorView 切到该 app"的导航路径覆盖。
- [ ] iOS 下 `eka2l1::drivers::graphics::make_gl_context` 返回真实 EAGL 上下文实现，不再走 nullptr 兜底；ogl 后端在 iOS 上编进 drivers 并被 graphics_driver 正常实例化。
- [ ] App 进入后台 (`UIApplicationWillResignActive`) 时模拟器暂停、回前台时恢复，不出现因丢失 GL 上下文的渲染崩溃。
- [ ] 阶段期间为修复 iOS 编译/运行问题打的 patch 集中记录在本阶段末尾的"阶段 2 修复清单"，沿用 0.7 / 1.x 的体例。

### 子任务

#### 2.1 ogl 后端在 iOS 上的最小可编
- [x] `src/emu/drivers/CMakeLists.txt`：`DRIVERS_OGL_SRC` 提到三平台共用；iOS 下 `target_link_libraries(drivers PRIVATE "-framework OpenGLES")` 直接链 system framework，glad 仍只在桌面/Android 链接。
- [x] 新增 `src/emu/drivers/include/drivers/graphics/backend/ogl/ios_gl_loader.h`，集中：①`#include <OpenGLES/ES3/gl.h>` + `<OpenGLES/ES3/glext.h>`；②`glad_glGetError` / `glad_glLineWidth` 直通真实 GLES 入口；③`gladLoadGL` / `gladLoadGLES2Loader` / `glad_set_post_callback` 写成 no-op；④补齐 `GL_BGRA` / `GL_BGR` / `GL_LINE_SMOOTH` / `GL_MULTISAMPLE` / `GL_SAMPLE_ALPHA_TO_ONE` / `GL_TEXTURE_1D` / `GL_TEXTURE_BINDING_1D` / `GL_TEXTURE_BORDER_COLOR` / `GL_SAMPLER_1D` / `GL_GEOMETRY_SHADER`；⑤inline shim：`glClearDepth` → `glClearDepthf`、`glDepthRange` → `glDepthRangef`、`glDrawBuffer` → `glDrawBuffers(1, …)`、`glPolygonMode` no-op、`glDrawElementsBaseVertex` 丢弃 base vertex 走 plain `glDrawElements`、`glTexImage1D` / `glTexSubImage1D` / `glCompressedTexImage1D` / `glCompressedTexSubImage1D` 全部空实现（Symbian 内容不会真的走 1D 纹理路径）；⑥顶端 `#define GLES_SILENCE_DEPRECATION` 抑制 iOS 12+ 噪音。
- [x] 八个 ogl 源/头里所有 `#include <glad/glad.h>` 改成 `#if EKA2L1_PLATFORM(IOS) ios_gl_loader.h #else glad.h #endif`，桌面/Android 路径完全不变。`graphics_driver_shared.cpp` 已是 iOS-exclude，留作 no-op。
- [x] 工厂层 `buffer.cpp` / `graphics.cpp` / `shader.cpp` / `fb.cpp` / `texture.cpp` / `input_desc.cpp` 中阶段 0 留下的 `#if !EKA2L1_PLATFORM(IOS)` 守卫全部撤掉，让 iOS 也走 `case graphic_api::opengl: return std::make_unique<ogl_*>(...)` 实分支。
- [x] 真实构建验证：`scripts/build_ios.sh simulator` 通过；`scripts/build_ios.sh smoke` 仍打 `EKA2L1_SMOKE: PASS backend=dyncom instrs=9 pc=0x00001024`，桌面 GL 假设并未泄漏到 dyncom smoke 路径。

#### 2.2 EAGL graphics context
- [x] 新建 `src/emu/drivers/src/graphics/backend/context_eagl.{h,mm}`：`gl_context_eagl` 持有 `EAGLContext *`（优先 `kEAGLRenderingAPIOpenGLES3`，失败回落 GLES2）+ `CAEAGLLayer *`，自行管理 framebuffer / colorRenderbuffer / depth-stencil renderbuffer。`render_surface == nullptr` 时降级 headless，只建 FBO 备用。
- [x] `attach_layer` 走 `[EAGLContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:]` 绑 layer；从 renderbuffer 反查 width/height 作为 `m_backbuffer_*`；挂 `GL_DEPTH24_STENCIL8` 到 depth/stencil attachment。
- [x] `swap_buffers` = `presentRenderbuffer:GL_RENDERBUFFER`；`make_current` / `clear_current` 走 `[EAGLContext setCurrentContext:]`。`update(w,h)` 重新 attach layer（renderbuffer 跟随 drawable size）。
- [x] `pause()` 在持有 context 的线程上 `glFinish` 再 `setCurrentContext:nil`；`resume()` 重新 setCurrentContext。后台回前台时给前端 hook（任务 2.9 调用）。
- [x] `update_surface(void*)` 允许 EAGLView 重建后换 layer。
- [x] `context.cpp` 在 `EKA2L1_PLATFORM(MACOS)` 之前优先匹配 `EKA2L1_PLATFORM(IOS)`，返回 `gl_context_eagl`；AGL 路径不动。`window_system_type::iOS` 加入枚举。
- [x] `CMakeLists.txt` iOS 分支编译 `context_eagl.{h,mm}`，并链 `QuartzCore` / `Foundation` / `UIKit`（除原本就有的 OpenGLES.framework）。simulator 构建通过。

#### 2.3 iOS emu_window
- [x] 新建 `src/emu/drivers/include/drivers/graphics/backend/emu_window_ios.h` + `src/emu/drivers/src/graphics/backend/emu_window_ios.mm`：继承 `drivers::emu_window`，全部纯虚的桌面/触屏 API 给出 no-op 实现（cursor / fullscreen / poll_events / change_title 等）。
- [x] `surface_changed(void *layer, int w, int h, float scale)`：iOS 前端在 EAGLView 的 `layoutSubviews` 中调用，更新 layer 指针 + framebuffer 像素尺寸 + scale，再触发 `surface_change_hook` 与 `resize_hook`，把新 surface 透到 EAGL context（任务 2.7 接入）。
- [x] `get_window_system_info()` 返回 `window_system_type::iOS` + render_surface（CAEAGLLayer 指针）+ surface_width/height + render_surface_scale，供 EAGL context 初始化使用。
- [x] `window_size()` 用 fb_size/scale 换算 logical points，`window_fb_size()` 返回像素尺寸；mouse 路径返回 0（pointer 事件走 `IosEmulator::submit_pointer_event`，任务 2.8）。
- [x] `CMakeLists.txt` 把 `emu_window_ios.{h,mm}` 一并塞进 iOS 分支的 drivers target；simulator 构建通过。

#### 2.4 iOS 端 emulator state 对象
- [x] 新建 `src/emu/ios/Bridge/IosEmulator.{h,mm}`：Obj-C facade `EKA2L1Emulator` (singleton) + 私有 C++ `eka2l1::ios::emulator` struct，字段 `unique_ptr<system> symsys` / `unique_ptr<config::app_settings>` / `unique_ptr<drivers::emu_window_ios>` / `drivers::graphics_driver_ptr` / `config::state conf`。无 audio / sensor / camera / vibration 字段（阶段 3）。
- [x] facade API：`+shared` 单例、`-startWithDocumentsPath:`、`-shutdown`、`-availableRoms`、`-mountRomNamed:`、`-rescanApps`、`-launchAppWithUID:`、`-installSisAtPath:`、`-attachLayer:pixelSize:scale:`、`-pause` / `-resume`、`-submitPointerEventAtX:y:phase:pointerId:`。`EKA2L1AppEntry` 类用于跨边界传 `(uid, name)`。
- [x] `startWithDocumentsPath:` 建 `<Documents>/{roms,data,sis,data/drives/{c,d,e,z},data/compat}` 目录，`chdir(<Documents>/data)` 之后初始化 `log::setup_log`、deserialize config、把 `conf.storage` 指向 sandbox data 目录，再构造 `eka2l1::system`（audio_ / graphics_ 都置 nullptr）。
- [x] `availableRoms` 扫 `<Documents>/roms` 一级子目录返回名字数组；`mountRomNamed:` / `rescanApps` / `launchAppWithUID:` / `installSisAtPath:` / `submitPointerEventAtX:...` 当前是占位 stub，留待 2.6 / 2.8 / 2.9 真实接入。
- [x] `attachLayer:pixelSize:scale:` 转发到 `emu_window_ios::surface_changed`，把 layer 指针推到 `window_system_info::render_surface`；graphics_driver 实例化与 emu 线程在 2.9 接入。
- [x] Bridging-Header 加入 `IosEmulator.h`；CMake 把 `IosEmulator.{h,mm}` 编进 iOS bundle。
- [x] 链接闭环修复：①新增 `src/emu/common/src/ios/applauncher.mm`，`eka2l1::common::launch_browser` 走 `UIApplication openURL`（host_launch.o 一直引用该符号，桌面有 Qt 版、Android 有 JNI 版，iOS 之前缺）；②新增 `src/emu/drivers/src/ui/input_dialog_ios.cpp`，`drivers::ui::open_input_view` / `close_input_view` / `show_yes_no_dialog` 暂作 no-op，让 dispatch 层的 ehui_* 链接通过（真实弹窗推迟到阶段 3）。simulator build 通过。

#### 2.5 ROM / 数据布局
- [x] sandbox 目录结构写进 `IOS_PORTING_PLAN.md` 阶段 2 章节："iOS sandbox 目录布局（任务 2.5 定稿）"：`Documents/{roms/<rom>,data/{drives/{c,d,e,z},compat,config.yml,EKA2L1.log},sis/}`，附 host 路径取法。
- [x] `IosEmulator::startWithDocumentsPath:` 自动建出全部缺失子目录（roms / data / sis / drives/{c,d,e,z} / compat），然后 `chdir(<Documents>/data)` 并把 `conf.storage` 指向同路径。前端从此不再依赖 cwd 假设。
- [x] 新增 `scripts/seed_ios_simulator_documents.sh`：把本地 `roms/` 一级子目录用 `rsync -a --delete` 同步到 `xcrun simctl get_app_container booted <bundle> data/Documents/roms/`，顶层 `*.sis` / `*.sisx` 同步到 `…/Documents/sis/`；支持 `--dry-run`，含路径含空格的目录正确加引号；booted 模拟器缺失或 EKA2L1 未装时报错退出。
- [x] 回归：`scripts/build_ios.sh smoke` 仍 PASS，证明 sandbox bootstrap 没有影响 stage-1 通路。

#### 2.6 ROM 加载与 applist 扫描
- [x] `-mountRomNamed:`：把 `<Documents>/data/drives/z/` `symlink → <Documents>/roms/<name>/`（避免几百 MB 的拷贝），再依次 `rescan_devices(drive_z)` / `startup()` / `set_device(0)` / 挂 C/D/E/Z / `initialize_user_parties()`，最后通过 `get_winserv_name_by_epocver` 抓 `window_server` 句柄缓存到 state 里（任务 2.8 的输入路径要用）。前提是 ROM 文件夹内已经是 desktop 安装好的 device 树（含 `devices.yml`），与 Android 流程一致；没有 device 时返回 NO 让 UI 报错。
- [x] `-rescanApps`：通过 `kern->get_by_name<service::server>(get_app_list_server_name_by_epocver(...))` 抓 `applist_server`，调 `rescan_registries(io)`，遍历 `get_registerations()`，跳过 `caps.is_hidden`，把 `(uid, long_caption→UTF8)` 装进 `EKA2L1AppEntry` 数组返回 Swift。Icon / SVG 渲染留给阶段 3。
- [x] `-launchAppWithUID:`：拿 `get_registration(uid)`，构造 `epoc::apa::command_line{ launch_cmd_ = command_create }`，`kern->lock()` → `alserv->launch_app(*reg, cmdline, nullptr, nullptr)` → `kern->unlock()`，返回结果布尔。
- [x] `-installSisAtPath:`：调 `symsys->install_package` 装到 E 盘（S80 则装 D），结果转 `installation_result_success` 判断。
- [x] simulator build 通过；运行时是否能真正拉起一个 GUI 应用要等任务 2.7 / 2.9 装好渲染管线后才能验证，stage-2 验收阶段最后跑 manual 走查。

#### 2.7 SwiftUI 外壳与 EAGL 视图
- [x] 重写 `src/emu/ios/App/ContentView.swift`：`NavigationStack` 三屏 — ROM 列表（`Documents/roms` 一级目录）→ App 列表（`mountRomNamed:` + `rescanApps` + "Install SIS" 列出 `Documents/sis/` 下 `.sis/.sisx`）→ EmulatorView。原 CPU smoke UI 移到 "Diagnostics" 二级页。booting 时调 Swift facade `EKA2L1Bridge.shared.start(documentsPath:)`，失败展示 banner。
- [x] 新建 `src/emu/ios/App/EKA2L1Bridge.swift`：Swift 6 前端 facade，集中封装 `EKA2L1Emulator` / `EKA2L1CpuSmokeBridge` / config / haptics / input / icon / smoke 调用；SwiftUI 层只依赖 Swift value types（`EKA2L1AppItem` / `CpuSmokeReport`），ObjC 只保留 C++ 桥接边界。
- [x] 新建 `src/emu/ios/App/EmulatorView.swift`：`UIViewControllerRepresentable`，把 UID 透到 Swift `EmulatorViewController`。
- [x] `src/emu/ios/App/EmulatorViewController.swift` 替换旧 `Bridge/EmulatorViewController.{h,mm}`：内部 `EKA2L1RenderView : UIView`，`layerClass = CAEAGLLayer`，`contentScaleFactor = UIScreen.main.nativeScale`，`isOpaque = true`；`layoutSubviews` 算出像素尺寸后通过 `EKA2L1Bridge.attach(layer:pixelSize:scale:)` 推回 IosEmulator。`viewDidAppear` 在 layer ready 后 `launchApp(uid:)` + `resume`；`viewWillDisappear` `pause`。
- [x] Bridging header 不再暴露 UIKit 控制器；CMake bundle 编进 Swift bridge / Swift controller，并设置 `SWIFT_VERSION=6.0`。`IosEmulator.h` 给 Swift 调用面补 `NS_SWIFT_NAME`，避免 Swift 侧依赖 ObjC selector 细节。
- [x] 单指触控派发到 `EKA2L1Emulator::submitPointerEventAtX:y:phase:pointerId:`（任务 2.8 的派发链路在这里就位，IosEmulator 内部转发到 window_server 也在 2.8 完成）。

#### 2.8 输入：触控 → pointer_event
- [x] Swift `EKA2L1RenderView` 重写 `touchesBegan/Moved/Ended/Cancelled`：对每个 `UITouch` 抽 `location(in:)`，乘 `contentScaleFactor` 转 framebuffer 像素，phase 映射 `UITouch.Phase → EKA2L1PointerPhase`，`pointerId = ObjectIdentifier(touch)` 单调可比，调 `EKA2L1Bridge.submitPointer(x:y:phase:pointerId:)`。
- [x] 阶段 2 初始只要求单指；阶段 3 已把 `isMultipleTouchEnabled = true`、长按、pinch 与虚拟键盘接到 Swift 控制器。
- [x] `IosEmulator::submitPointerEventAtX:...` 构造 `drivers::input_event{ type_=touch, mouse_=…, raw_screen_pos_=true, mouse_id=pointerId&0xFFFFFFFF }`，phase 映射 `mouse_action_press/repeat/release`，调 `_state->winserv->queue_input_from_driver(evt)`。`winserv` 句柄是任务 2.6 `mountRomNamed:` 时从 `kern->get_by_name<service::server>(get_winserv_name_by_epocver(...))` 拿到的，未挂 ROM 时 silently drop。
- [x] 不接键盘 / 物理键盘 / 游戏手柄；阶段 4 与发布通道一起做。simulator build 通过。

#### 2.9 帧循环与生命周期
- [x] `startWithDocumentsPath:` 末尾 spawn 两条 `std::thread`：①graphics_thread —— 在 `layer_cv` 上等到 attachLayer 首次推 layer，把 layer 透给 `emu_window_ios::surface_changed`，然后在本线程创建 `drivers::create_graphics_driver(opengl, info)`（这样 EAGL context 与 ogl driver `run()` 都绑定在同一条线程），注册 `surface_change_hook` / `set_display_hook`（presentRenderbuffer 已经在 `gl_context_eagl::swap_buffers` 内部，hook 为空），最后调 `graphics_driver->run()` 阻塞驱动循环。②os_thread —— `while(running) symsys->loop()`；`paused` 时 sleep。
- [x] `attachLayer:pixelSize:scale:`（主线程调用）在 layer_mutex 下把指针/尺寸/scale 推到 pending_* 字段并 `notify_all`，graphics_thread 首次启动后续 layout 变更则走 `emu_window_ios::surface_changed → surface_change_hook → graphics_driver->update_surface`，避免再次跨线程构造 driver。
- [x] `mountRomNamed:` 成功后给每块 `epoc::screen` 注册 `add_screen_redraw_callback`，回调里 `wait_for(&present_status)` → `graphics_command_builder.present(&present_status)` → `submit_command_list`，让窗口服务的画帧节拍触发 EAGL swap。
- [x] `EKA2L1App` 用 `@Environment(\.scenePhase)` 监听 `.active / .inactive / .background`：active → `EKA2L1Emulator::resume`、其它 → `pause`。`pause` 标志被 os_thread 与 swap_buffers 双重尊重；graphics_driver::pause()/resume() 留给后续在 gl_context_eagl 上加 hook（当前 swap_buffers 已经检查 `m_paused`）。
- [x] `shutdown` 把 `running=false`、`layer_cv.notify_all`、`graphics_driver->abort()`，再 join 两条线程，安全释放 symsys / driver / window。
- [x] **stage-1 回归**：阶段 2 UI 重构后 `scripts/build_ios.sh smoke` 不再因为 ContentView 不自动跑 smoke 而 timeout —— 改在 `EKA2L1App.init()` 里 background dispatch 一次 `EKA2L1CpuSmokeBridge`，PASS 仍打 `EKA2L1_SMOKE: PASS backend=dyncom instrs=9 pc=0x00001024`。simulator + smoke 双绿。

#### 2.10 验证脚本
- 不要求阶段 2 做端到端的"无人值守"自动验证（无法可靠 grep 出"出图了"）；保留 `scripts/build_ios.sh smoke` 的语义不变，只验 CPU smoke 仍然通过——这是回归网。
- 新增 `scripts/seed_ios_simulator_documents.sh`：把 `roms/N95 8GB (S60v3 - FP1)`、`roms/snakes-n95_n6trsohu.sis` 同步到当前 booted 模拟器的 EKA2L1 sandbox Documents 下，方便复跑。
- 手工验收步骤记录在本阶段末尾（截屏 + 日志）。

#### 2.11 文档与遗留项
- [x] 阶段 2 修复清单已就位（见下文 10 条），沿用 0.7 / 1.x 体例。
- [x] 重构动作明确推到阶段 3：①真正的 ROM 安装流程（替代 symlink + 期望 desktop 预装 device）；②launcher::draw 等价的 iOS 复合渲染（背景 / letterbox / 缩放 / 旋转）；③SVG/MIF 图标解码 + icon UI；④UIDocumentPicker SIS 导入；⑤UIAlertController 真实输入对话框；⑥cubeb iOS 后端 + Core Haptics 振动；⑦多指 / 长按 / 屏幕键盘。
- [x] 变更日志补一条 2026-05-22 收尾。

### 阶段 2 修复清单（按出现顺序）
1. **`GL_BGR` / `GL_BGRA` / `GL_LINE_SMOOTH` / `GL_MULTISAMPLE` / `GL_SAMPLE_ALPHA_TO_ONE` / `GL_TEXTURE_1D` / `GL_TEXTURE_BINDING_1D` / `GL_TEXTURE_BORDER_COLOR` / `GL_SAMPLER_1D` / `GL_GEOMETRY_SHADER`**：GLES3 头文件不存在。集中在 `ios_gl_loader.h` 里给等价或占位常量，让 ogl backend 的 enum 表能编通；运行期那些功能不会被打开（feature gate 已隔离）。
2. **`glClearDepth` / `glDepthRange` / `glDrawBuffer` / `glPolygonMode` / `glDrawElementsBaseVertex` / `glTex(Sub)?Image1D` / `glCompressedTex(Sub)?Image1D`**：GLES3 缺失。`ios_gl_loader.h` 内 inline shim：`f`-后缀别名 / `glDrawBuffers(1, …)` / no-op / 丢 baseVertex / 1D 全空实现。Symbian 内容不会真的踩 1D 纹理；线框 / draw-buffer / depth-range 已不再用。
3. **glad 接口直通**：把 `glad_glGetError` / `glad_glLineWidth` 在 iOS 下 `#define` 到 GLES 入口；`gladLoadGL` / `gladLoadGLES2Loader` / `glad_set_post_callback` / `GLADloadproc` 全部 no-op，因为 iOS 上 OpenGLES.framework 是直接链接的。
4. **iOS 12+ GLES deprecation**：`ios_gl_loader.h` 顶端 `#define GLES_SILENCE_DEPRECATION` 抑制告警噪音。
5. **`gl_context_eagl::attach_layer` 在 SwiftUI 主线程被构造会拿不到 EAGLContext 所有权**：把 `create_graphics_driver` 推到 graphics_thread，layer 指针通过 condition_variable 跨线程递交，保证 context / FBO / present 都在同一条线程上。
6. **`drivers::ui::open_input_view` / `close_input_view` / `show_yes_no_dialog` 链接缺失**：dispatch 层的 `ehui_*` 引用这三个符号，Android/Qt 各自实现。新增 `drivers/src/ui/input_dialog_ios.cpp` 暂作 no-op，真实 UIAlertController 留给阶段 3。
7. **`common::launch_browser` 链接缺失**：`host_launch.o` 引用，Android 走 JNI，Qt 走 `QDesktopServices`。iOS 新增 `common/src/ios/applauncher.mm` 走 `[UIApplication openURL:options:completionHandler:]`。
8. **smoke 自动回归丢失**：阶段 2 把 ContentView 从 "默认跑 smoke" 改成 "默认进 ROM 列表"，导致 `scripts/build_ios.sh smoke` 30s timeout。改在 `EKA2L1App.init()` 里 background dispatch 一次 `EKA2L1CpuSmokeBridge` 并 NSLog `EKA2L1_SMOKE: ...` 标记，UI 流程保持新的 stage-2 三屏。
9. **`drive_number` 命名空间**：阶段 0 写的 `eka2l1::drive_number::drive_d` 在 iOS path 触发 "no type named 'drive_number' in namespace 'eka2l1'"。`drive_number` / `drive_z` 等都是顶层枚举，去掉冗余的 `eka2l1::` 限定即可。
10. **`present_status` 类型**：第一版写成 `std::atomic<int>` 想跨线程，但 `graphics_command_builder::present(int*)` 要的是裸 `int*`。还原为 `int` 字段，依赖窗口服务回调串行化保护。
11. **os_thread 在 mount 之前跑 `symsys->loop()` → SIGSEGV**：xcodebuildmcp 装好 .app 后启动立刻 crash。Crash report 显示 `kernel_system::crr_thread()` 在 `unique_ptr<thread_scheduler>::operator->` 上 deref null —— `system_impl::loop()` 在 `set_device` / `startup` 之前不能跑。补一个 `std::atomic<bool> mounted{false}`，os_thread 未 mount 时只 sleep；`mountRomNamed:` 末尾翻为 true。修复后 simulator 启动稳定，主屏 → AppListView → Mount 全程不再 crash。
12. **ROM mount 用 symlink graft 时 `rescan_devices` 的 "broken device" cleanup 会沿 symlink 删用户 ROM 源文件**：早期 mount 把 `Documents/data/drives/z → roms/<rom>/data/drives/z` 整体软链，`rescan_devices` 检测到 SYM.ROM 不在期望路径时调用 `delete_folder(full_entry_path)`，POSIX `remove_all` 跟随 symlink 把源 8000+ 文件清掉。改成完全跳过 `rescan_devices`：seed 阶段写好 `devices.yml`，mount 直接 `startup` → `set_device(0)` → `mount(...)`。
13. **iOS sim app 容器的 case-sensitivity 是单边的**：host APFS 卷case-insensitive（shell 视角看 `RM-320` / `rm-320` 同一 inode），但 iOS sim runtime 给 app 进程的是 case-sensitive 视图（`fopen("rm-320")` 返 ENOENT 当真名是 `RM-320`）。无法同时存在两种 case 的条目（host 层 EEXIST 拒绝）。系统代码大量用 `common::lowercase_string(firmcode)` 拼路径，所以 sandbox 内 dirs 一律用**小写**：`data/drives/{c,d,e,z}`、`data/roms/<lower-firmcode>`、`SYM.ROM`（系统硬编码大写）。`sys->mount(...)` 也跟着传小写路径。
14. **stage-2 mount 链路真实跑到 kernel chunk allocation 的 SIGBUS**：经过上面所有修复后，xcodebuildmcp 自动化点 Mount 流程现在能跑完 `mountRomNamed:` → 重建 system → `startup()` → `set_device(0)` → 进 `reset` → 映射 ROM 到 `0x11f904000` → 创建 ROM chunk → 创建 kernel-data chunk → 进 `dispatcher` 初始化分配 chunk 时 `std::fill_n` 写 0 触发 `SIGBUS KERN_PROTECTION_FAILURE`。这是 stage-3 才该解决的 iOS sandbox mmap 限制（kernel memory model 在 host 上拿到的虚拟页不可写），与 stage-2 的"前端 → mount → applist → 帧 → 触控"管线本身无关。stage-2 acceptance 的"applist ≥ 5 / 出一帧 / 点 Calculator" 仍需 stage-3 把 kernel chunk 分配迁到 iOS 友好的 mmap 路径（MAP_JIT / 显式 mprotect / 自管 chunk 分配器）后才能跑通。

### 阶段 2 已知风险
- **OpenGL ES on iOS 已 deprecated 但仍可用**：iOS 12+ 至今 SDK 仍带 OpenGLES.framework，但 Apple 偶尔在新 SDK 提高警告等级。验收期内（iOS 18 / Xcode 26 序列）确认 OK；长期方向是 MoltenVK / Metal，留给后续阶段。一旦 SDK 真的拿掉 OpenGLES，本阶段产物会一起失效，但这是已知交换。
- **ogl 后端隐含的桌面 GL 假设面积可能比当前列出的大**：解码路径里的 `GL_BGRA`、纹理 swizzle、PBO upload、debug callback 等都可能命中。原则同阶段 1 ——就地最小修复 + `// TODO(ios)`，不重构。
- **EAGL 上下文与线程绑定严格**：一旦把 emu 线程切到别处或在主线程偷偷调 GL，会拿到 `INVALID_OPERATION` 或直接 crash。所有 GL 调用必须严格走 emu 线程；后台/前台切换路径要在第一版就写对。
- **applist 在 iOS 上首次扫描可能因 vfs/路径假设暴露 Linux/Android 专属代码**：`load_registry` 系列在 case-sensitive FS 下读 Z 盘资源时，路径大小写差异可能导致扫描结果为空。优先复用 Android 的 `vfs` 处理，避免阶段 2 引入新 vfs 行为。
- **Documents 沙盒路径含空格**：`N95 8GB (S60v3 - FP1)` 这种带空格的目录名会让 shell 脚本 / 部分 C++ 路径拼接出问题。`seed_ios_simulator_documents.sh` 必须正确引用，C++ 侧确认 `eka2l1::common::utf8_to_utf16` + `loader` 不在路径里强行 split。
- **不做 audio 的情况下 services 启动顺序可能依赖音频初始化完成的信号**：阶段 0 已经让 `make_audio_driver` 在 iOS 返回 nullptr，但 services 内部 `if (audio_driver)` 检查不一定齐全，可能在 applist / mediaclient 启动路径上踩 nullptr。命中时就地最小修复，不要在阶段 2 改音频架构。

---

## 阶段 3：解锁 mount 链路 + 完成阶段 2 验收 + 完整体验

### 目标
两件事并轨推进：

1. **解阶段 2 遗留**：阶段 2 修复清单第 14 条的 kernel chunk SIGBUS 把 mount → applist → 出帧 → 触控 这条主线管线堵在了 "chunk 初始化写 0" 那一步，导致阶段 2 的真实验收（applist ≥ 5、Calculator 出一帧、单指交互、SIS 安装后出现在 applist）至今没法跑通。阶段 3 必须先把这个 mmap/mprotect/W^X 的 mismatch 解掉，然后回头补完阶段 2 的真实验收，再继续往下。
2. **完整体验**：在 mount 链路彻底通畅后，按原计划把音频（cubeb AudioUnit）、振动（Core Haptics）、文件导入（UIDocumentPicker）、SVG/MIF 图标、设置面板、UIAlertController 输入、多指/手势、真正的 ROM 安装流程、字体导入引导逐项接齐。

阶段 4 的 dynarmic / JIT / W^X / entitlement / 发布通道**仍然不在范围内** —— 阶段 3 只在解 3.1 的 chunk 分配问题时，会顺手把 `common::virtualmem` 的 "非可执行内存" 路径理顺；可执行内存（MAP_JIT）继续推迟到阶段 4，与签名通道一起做。

### 验收标准
- [ ] **阶段 2 acceptance 补完**：N95 ROM 在 booted 模拟器和 iOS 真机上挂载后，applist 列出 ≥ 5 个内建应用、与 Qt / Android 前端在同一 ROM 下的列表对得上；点击 Calculator（或 Notes / Calendar，任一不依赖音频的 GUI 内建应用），EAGL 渲染面稳定刷出 ≥ 1 帧真实画面、无 crash 持续 ≥ 10s；单指 tap 把 `1 + 1 =` 算出来。
- [ ] **SIS 安装链路真实跑通**：通过 UIDocumentPicker 或手工放进 `Documents/sis/`，`snakes-n95_n6trsohu.sis` 装到 E 盘、出现在 applist 并能 launch 到游戏主菜单（游戏内逻辑跑不下去不计入失败）。
- [ ] **音频**：cubeb iOS AudioUnit 后端在 iOS 重新进 build graph，`make_audio_driver` 返回真实实例；一个带 BGM 或 SFX 的 ROM 应用（候选：N-Gage 试玩 / 一个有声音的 Symbian 小游戏）能听见声音。
- [ ] **振动**：`drivers::vibration` 在 iOS 下走 `CHHapticEngine`（或 `UIImpactFeedbackGenerator` fallback），有一个触发振动的应用能感受到反馈，无 crash。
- [x] **文件导入**：`.fileImporter`（UIDocumentPicker）+ Info.plist 文件关联打通；SIS 经 `ImportRouter` 落 `Documents/sis/`、ROM/RPKG 经 `ImportDeviceView` 走真实安装（详见 3.5 / 3.4），前端 UI 即时刷新。"Share to EKA2L1" extension 推迟（见 3.5 follow-up）。
- [ ] **AppList 图标**：applist 显示真实 SVG/MIF 图标（解码走 lunasvg / mif decoder），不再是占位。
- [ ] **设置面板**：SwiftUI 设置页覆盖 `config::state` 主要字段（device 切换、屏幕方向、上采样、音量、按键映射），改动持久化到 `config.yml`。
- [ ] **输入对话框**：阶段 2 留下的 `input_dialog_ios.cpp` no-op 替换为真实 `UIAlertController` 实现（包括 `open_input_view` / `close_input_view` / `show_yes_no_dialog`）。
- [x] **真正的 ROM 安装流程**：取消 symlink / hardlink graft + 期望 desktop 预装 device tree 的权宜方案，改走 Android / macOS 同款 `install_rom` / `install_rpkg` + `save_devices()`（详见 3.4）。
- [ ] **字体导入引导**：检测到 ROM 缺字体时，UI 引导用户通过 DocumentPicker 添加 `.ttf`，拷到 `data/fonts/` 并被 freetype 拾取。
- [ ] **多指 / 手势**：`multipleTouchEnabled = YES`，至少跑通双指（用于 launcher 缩放或屏幕键盘场景）和长按手势。
- [ ] 阶段 3 修复清单按出现顺序登记（同 0.7 / 1.x / 2.x 体例）。

### 子任务

#### 3.1 解锁 mount 链路：iOS 内核 chunk 写 0 SIGBUS ✅
- **核心结论**：mount 清零写触发 SIGBUS，根因是 Apple Silicon 用 16 KB host page 而 EKA2L1 按 4 KB 粒度调 `mprotect` 被 kernel 静默拒绝；修法是把 `common::virtualmem` 的 protect 范围对齐到 host page size。
- 详见 [`ios-mount-chunk-sigbus.md`](./ios-mount-chunk-sigbus.md)（含定位现场、候选 root cause、分层修法与验收）。

#### 3.2 阶段 2 验收最后一公里 🟡
- 3.1 + 3.2.1 通畅后，xcodebuildmcp 自动化跑：booted iPhone 16 Pro 模拟器上启动 → 进 ROM 列表 → 选 N95 → Mount → 等 applist 渲染 → 校验 entry 数 ≥ 5、能看到 Calculator / Notes / Calendar / Camera / Contacts 这类典型名字 → 点 **Calculator** → 等渲染面出非清屏色 → 单指 tap `1 + 1 =` → 截屏归档。
- SIS 安装验证：使用 **`The Final Battle.sis`** 替换原计划的 `snakes-n95_n6trsohu.sis`（N95 ROM 上 Snake 启动到主菜单后被自身的 codeseg 兼容性 bug 卡住，与 iOS 端无关）。把 The Final Battle.sis 拷进 `Documents/sis/`，UI 上点 "Install SIS" → applist 出现新条目 → launch → 截屏归档。
- 把这次手工验收的截屏与日志归档到 `docs/screenshots/ios-stage3/2-acceptance/`（沿用 stage-2 的 archive 结构），并把 stage 2 状态从 🟡 翻 ✅；阶段总览表 + 阶段 2 验收复选框相应更新。
- 不要求阶段 3 端到端无人值守自动化，`scripts/build_ios.sh smoke` 仍只验 CPU smoke 不退化。
- **当前状态（2026-05-23 evening）**：✅ **3.2 关闭**。iPhone 16 Pro 模拟器上 mount N95 → applist 63 app；点 Calculator → EmulatorView 出 Calculator 真实 UI（截屏 `docs/screenshots/ios-stage3/2-acceptance/calculator-rendered.jpg`），稳定 ≥12s；点 The Final Battle.sis → applist 出 `Final Battle, uid=0xA0003C62`（截屏 `applist-with-final-battle.jpg`）；点 Final Battle button → EmulatorView 出 FBattle launch（截屏 `final-battle-launched.jpg`，FBattle 进程持续 108+ 次线程调度，音频缺失导致 hssSDthread 退出但游戏主线程仍活）。剩余 follow-up：3.11 虚拟键盘后才能完成 Calculator 的 `1 + 1 =` 数字输入交互（S60v3 数字键没有 on-screen 按钮）。

#### 3.2.1 app launch 后 guest "Main" 线程 PC=0 死循环 ✅（host-page 对齐后自动解决）
- **核心结论**：与 3.1 同根——16 KB host page 下按 4 KB 调 `mprotect` 被静默拒绝，留在 `PROT_NONE` 的栈页被弹栈即 SIGBUS、dyncom 续解码零字节就表现为"PC=0 沿 page 扫描"；提交 `779061f27` 对齐 protect 范围后自动解决。
- 详见 [`ios-guest-pc0-deadloop.md`](./ios-guest-pc0-deadloop.md)（含完整事实链、跨系统对比定位与修法）。

#### 3.2.2 iOS Calculator 已启动但 EAGL 仍无真帧 ✅
- **核心结论**：洋红空帧根因是 ogl 后端把 swapchain framebuffer 硬编码为 `glBindFramebuffer(GL_FRAMEBUFFER, 0)`，而 iOS GLES 无默认 FBO（命令静默丢弃、drawable 保留 Apple 洋红未初始化色）；修法给 `gl_context` 加 `virtual swapchain_framebuffer()`、由 EAGL context 返回挂着 CAEAGLLayer colorbuffer 的内部 FBO，桌面/Android 仍返回 0。期间另修三类缺省 driver 空指针崩溃 + present 时序对齐。
- 详见 [`ios-calculator-eagl-magenta.md`](./ios-calculator-eagl-magenta.md)（含 `_E32Startup` early-cleanup 排查、保留修复清单与最终验证截屏）。

#### 3.2.3 SIS 安装在 iOS 上静默写失败 ✅
- **核心结论**：`sis_script_interpreter` 把已解析的 **host 绝对路径**整条小写，iOS 容器路径含 `/Users/…` 和 UUID，小写后变成不存在的 `/users/…` 致 payload 写失败（registry 却成功 → "装好但 E 盘空"假阳性）；修法是只小写 Symbian 虚拟路径再解析、绝不小写 host 路径，并让 `get_raw_path` 失败时传播 false。
- 详见 [`ios-sis-install-write-fail.md`](./ios-sis-install-write-fail.md)（含现象、范围判断与验收）。

#### 3.3 `common::virtualmem` 与 mem 模块的 iOS 落实 ✅
- ✅ `is_memory_wx_exclusive()` 的语义在头文件 doc 里写清楚（`src/emu/common/include/common/virtualmem.h`）：iOS / Apple Silicon 仍返回 true，但只对"host 真要执行"的 JIT block 生效；普通 data chunk 已经在 `translate_protection()` 里被 strip 掉 PROT_EXEC，走 plain RW。可执行内存（`map_executable` / `jit_write_protect` / MAP_JIT / `pthread_jit_write_protect_np`）继续保留在阶段 4，与 dynarmic 通路一起做。
- ✅ `src/emu/common/src/virtualmem.cpp` 的 iOS / macOS arm64 `commit()` / `change_protection()` 16 KB host-page 对齐分支加 `// TODO(ios)` 注释，引用 3.2.1 的来历；清理 3.2.1 阶段加的 `[commit-fail]` 调试 fprintf 与 `<cerrno>` / `<cstdio>` / `<cstring>` 包含。
- ✅ 桌面 / Android / Win32 路径未受改动；iOS simulator build 通过（`scripts/build_ios.sh` 替代之 `xcodebuildmcp simulator build` SUCCEEDED）。

#### 3.4 真正的 ROM 安装流程（取代 symlink / hardlink graft）✅
- **核心结论**：废弃早期 hardlink graft + 手写 `devices.yml` 的权宜方案，改走与 Android / macOS 完全一致的真实安装管线（`install_device` / `install_rom` / `install_rpkg`），设备选择持久化、下次自动 boot。
- 详见 [`ios-rom-install-pipeline.md`](./ios-rom-install-pipeline.md)（含安装 API、并发锁、端到端验证）。

#### 3.5 UIDocumentPicker 文件导入 🟡
- **核心结论**：首页重设计为以设备为中心，`ImportDeviceView` 走真实 ROM/RPKG 安装、`+` 走单一 `.fileImporter` 装 SIS（双 importer 会被 SwiftUI 丢弃，改 `pickTarget` 多路复用）；安全作用域 URL 必须先拷暂存。剩余 ZIP 解包 / Share extension 待办。
- 2026-07-06 回归修复：主界面后续叠回了 SIS 与 N-Gage 两个 `.fileImporter`，导致右上角 `+` 设置状态但系统 picker 不出现；已改回 `HomeImportTarget` + 单一 home importer 多路复用。
- 2026-07-08：从文件 App / "Open in" 打开 `.sis/.sisx` 现在会自动安装到当前设备（`.onOpenURL` + 待处理队列，冷启动/运行中均覆盖；无设备时仅提示）。SIS 安装为原地进行，不再经 `Documents/sis/` 暂存。详见变更日志。
- 详见 [`ios-document-picker-import.md`](./ios-document-picker-import.md)（含 Info.plist UTI、导入页与 follow-up）。

#### 3.6 AppList 图标（SVG / MIF 解码）✅
- **核心结论**：`iconPNGDataForUID:sizePx:` 按 Android 同款顺序解 `.mif`(lunasvg) / `.mbm` / fallback `get_icon`，缩放成方形 PNG 给 SwiftUI 懒加载；附带修了空格 caption 触发的 `pystr` 空串崩溃。
- 详见 [`ios-applist-icons.md`](./ios-applist-icons.md)（含解码链路、缓存与稳定性修复）。

#### 3.7 iOS 原生 AudioUnit 后端 🟡
- **核心结论**：弃用 cubeb shim，iOS 直接接 AURemoteIO + AVAudioSession（`audiounit_ios` 后端），`make_audio_driver(cubeb,…)` 在 iOS 下返回该后端；services 拿到真 audio_driver、加载三个 audio patch DLL。剩余听感与 MIDI bank 验证待办。
- 详见 [`ios-audiounit-backend.md`](./ios-audiounit-backend.md)（含后端结构、CMake 切分与验证）。

#### 3.7.1 iOS DSP / FFmpeg 回接 🟡
- **核心结论**：新增 `scripts/build_ios_ffmpeg.sh` 用 bundled FFmpeg source out-of-tree 构建 iOS static libs（不 dirty 子模块），CMake 在 `EKA2L1_HAVE_FFMPEG` 时编回 `dsp_ffmpeg` / `player_ffmpeg` / `video_player_ffmpeg`，否则保留 PCM16/PCM8 fallback。simulator 已回接，device 路径与真实解码验证待办。
- 详见 [`ios-dsp-ffmpeg.md`](./ios-dsp-ffmpeg.md)（含脚本、CMake 回接与验证）。

#### 3.8 振动：Core Haptics ✅
- ✅ 新建 `src/emu/drivers/{include,src}/hwrm/backend/vibration_ios.{h,mm}`：iOS 18+ 直接用 `CHHapticEngine` + continuous `CHHapticEvent`，把 HWRM `millisecs/intensity` 映射为 duration/intensity/sharpness；不再维护 iOS 18 以下 fallback。
- ✅ CMake iOS 分支链 `CoreHaptics`；`vibration.cpp` 工厂在 iOS 走 `vibrator_ios` 替换 stage-0 的 `vibrator_null`。
- ✅ `SettingsView` 暴露 **Test vibration** 手动触发 180ms haptic，方便真机验收。模拟器上 Core Haptics 不可用属于平台限制。

#### 3.9 多指 / 手势 🟡
- ✅ `EKA2L1RenderView.isMultipleTouchEnabled = true`，现有 `touchesBegan/Moved/Ended/Cancelled` 已对 `Set<UITouch>` 逐个转发，pointerId 继续使用 touch object identity。
- ✅ `EKA2L1RenderView` 加 `UILongPressGestureRecognizer`：长按映射为 `std_key_device_3`（window server 已把它折算成 select/enter）。
- ✅ `UIPinchGestureRecognizer`：pinch end 时按 scale 映射成 up/down raw key，作为 S60 列表和菜单的轻量手势入口。
- ✅ `EmulatorView` 增加可隐藏的虚拟键盘 overlay：方向键、select、左右软键、0-9、`*`、`#` 都走 `EKA2L1Bridge.tapRawKey` → `EKA2L1Emulator::tapRawKey` → `drivers::input_event_type::key_raw` → window server。数字键/`*` 对齐 Android 现有虚拟键盘，发送 ASCII `'0'..'9'` / `'*'`；`#` 发送 `0x7f`。
- ✅ `EKA2L1RenderView` 同步接入外接键盘 `UIPress`：数字键/`*`/`#`、方向键、Return/Escape 映射到 raw key。手柄仍推迟到阶段 4。
- ✅ 物理键盘映射补全（`scanCode(for:)`，对照 `services/window/keys.h` 的 `std_scan_code`）：字母 a–z → 大写 ASCII（EPOC 约定 scan code 即大写 ASCII，与数字同路径）；Space→0x05、Backspace→0x01、Tab→0x02（这三者不随 ASCII，按 `keyCode` 映射）；F1→左软键 0xA4、F2/Escape→右软键 0xA5、F3→绿色拨号 0xB4、F4→红色挂断 0xB5；Return/小键盘 Enter→select 0xA7。验证：N95 计算器按 F1 打开 Options 菜单（LSK）、Escape 关闭（RSK），回归 PASS=8。
- ✅ 运行验证：iPhone 16 Pro / iOS 26.5 模拟器 build-and-run → mount N95 → 打开 `Final Battle` → 语言菜单按虚拟键 `1` 进入主菜单，确认 Final Battle 不再黑屏且数字键路径可用。
- ✅ 虚拟键盘布局管理器：键盘相关代码抽到 `src/emu/ios/App/VirtualKeypad.swift`，新增 `KeypadLayout` 枚举（`full` 经典 = 软键+call/end+方向盘+数字九宫格全展示；`compact` 精简 = 现代化方向键盘，对纯方向操作的游戏更友好）。`SettingsView` 加 Picker 切换并经 `@AppStorage("ios.keypadLayout")` 持久化；`EmulatorView` 经 `KeypadLayout.resolve` 渲染。回归脚本以 `-EKA2L1RegressionMode 1`（落 NSArgumentDomain、不污染持久值）强制经典布局保证软键断言稳定。
- ✅ Compact 方向模式：左右软键固定在左上/右上角（`chevron.up.right.2` SF Symbol + 左侧镜像），切换钮在右下角（纯 SF Symbol，`square.grid.3x3.fill`↔`dpad.fill`）；中央加大方向盘（直径 188），热区沿圆环按对角四等分（`Sector`/`PadDividers` shape + `HoldableRawKey.hitShape: AnyShape`），按压高亮对应扇区。Compact 数字模式参考 iOS 拨号键盘：`NativeNumericPad` 用 flexible 三列圆形键铺满整宽，不再收缩在中间。经典布局沿用原 `DPad`/`NumericPad`。验证：模拟器跑 Calculator，方向/数字两态截图正常，回归 PASS=8。

#### 3.10 设置面板 ✅
- ✅ 新建 SwiftUI `SettingsView`，首页 toolbar 右上 gear 进入。
- ✅ `IosEmulator` 暴露 `currentConfigSnapshot` / `applyConfigSnapshot`，设置页可读写并 `config::state::serialize()` 回 `Documents/data/config.yml`。
- ✅ 已接字段：device display name、orientation（iOS 前端 `AppStorage`）、integer scaling、nearest filtering、master volume、virtual keypad、hide system apps、extensive logging、CPU backend、log filter、JIT stage-4 占位。
- 🟡 多数 emulator/service 级设置仍需重启或重新 mount 才完全生效；UI 即时保存配置，运行中即时生效只覆盖 master volume 与前端 overlay。

#### 3.11 UIAlertController 输入对话框 ✅
- ✅ `src/emu/drivers/src/ui/input_dialog_ios.cpp` 替换为 Objective-C++ `input_dialog_ios.mm`：`open_input_view` 在主线程弹 `UIAlertController(.alert)` + `UITextField`，提交时按 `max_len` 截断并回调 `std::u16string`。
- ✅ `close_input_view` dismiss 当前 alert；`show_yes_no_dialog` 弹两按钮 alert，button1/button2 分别回调 0/1。
- ✅ iOS simulator 编译链路已接好；Notes 等真实文本输入流程还需要后续运行时样本验证。

#### 3.12 字体导入引导
- 启动时检测 `data/fonts/` 是否为空且 ROM 字体缺失（freetype 无 face 时由 services/fbs 给信号）；若缺，AppList 顶部出现一条引导横幅 → 点击触发 3.5 的 DocumentPicker（限制为 `public.truetype-ttf-font` / `org.openfontformat.otf`）。
- 拷贝完成后调 fbs 重新扫描字体目录。

#### 3.13 文档与遗留项
- 阶段 3 修复清单（按出现顺序，体例同 0.7 / 1.x / 2.x）维护在本节末尾。
- 把 "dynarmic / MAP_JIT / `common::virtualmem::map_executable` / W^X 切换 / `pthread_jit_write_protect_np` / 各签名通道下的 JIT 启用方法 / CI / dyncom vs dynarmic benchmark" 统一推到阶段 4，确保 3.3 留下的可执行内存 API 骨架在阶段 4 直接接得上。
- 阶段 3 期间的截屏（mount 链路打通后的 applist、Calculator 出帧、Snakes 主菜单、设置面板等）归档到 `docs/screenshots/stage3/`，按子任务编号分目录。
- 变更日志补阶段 3 拆解条目与各阶段收尾条目。

### 阶段 3 修复清单（按出现顺序）
0. （占位，按出现顺序往下编号） **OGL 后端 `bind_swapchain_framebuf` / `bind_framebuffer(0)` 在 iOS 静默绑空 FBO 导致整屏洋红**（关闭 3.2.2，详见上文）。修法在 `gl_context` 基类加 `swapchain_framebuffer()` 虚函数（桌面默认 0），iOS 的 `gl_context_eagl` 返回内部 m_framebuffer（attach_layer 已把 EAGL drawable colorbuffer + depth/stencil 挂到该 FBO）；ogl 后端两处硬编码 FBO 0 改成走 `context_->swapchain_framebuffer()`。同时 `IosEmulator::submit_screen_frame` 的反向 `wait_for(&present_status)` 改回 Qt/Android 同款 `wait_for → set -100 → present`。仅影响 iOS，桌面 / Android 行为不变。
1. **`prot_read_write_exec` 在 iOS sandbox 下 `mprotect` 静默丢 W**（阶段 2 #14 SIGBUS 根因）：iOS W^X 下 `mprotect(R|W|X)` 返回成功但 W 被剥离、写入即 fault；dyncom 不需要 PROT_EXEC，故在 `translate_protection` 的 iOS 分支统一剥掉。详见 [`ios-mprotect-wx-strip.md`](./ios-mprotect-wx-strip.md)。
2. **iOS sandbox 内 vfs 路径解析 case-sensitive，混合大小写 ROM 文件名失败**：iOS 下整条路径被强制小写、host case-sensitive 找不到 `Wsini.ini` 等文件；修法在 `get_real_physical_path` iOS 分支逐级按 `compare_ignore_case` 解析回真实大小写。详见 [`ios-vfs-case-sensitive-path.md`](./ios-vfs-case-sensitive-path.md)。
3. **AppList 滚动到空格 caption 应用时必现 crash**：N95 applist 中 `uid=0x101F4CD2` 的 caption 是空格，图标懒加载走 MIF debinarize cache 时 `strip_reserverd().strip()` 产出空串，`pystr::rstrip()` 对空 string 调 `back()` 被 libc++ hardening abort。修法：`pystr::lstrip/rstrip` 加 empty guard；MIF cache 文件名为空时改用 `uid_<UID>`；iOS 图标解码入口加 mutex，避免滚动时多条 SwiftUI row 并发触碰 applist/fbs/io。验证：iPhone 16 Pro 模拟器上连续滚动 N95 AppList 到中后段和底部，无新 crash report。
4. **Final Battle 从列表底部打开黑屏**：Final Battle 进程启动后创建 DSP out stream，iOS 因为 FFmpeg 被 skip 而 `new_dsp_out_stream()` 返回空，日志出现 `Unable to create new DSP out stream!`，随后 `hssSDthread` 以 `KERN-EXEC` / access violation 退出，画面保持黑屏。阶段性修法：iOS 下先给 `dsp_stream_backend_ffmpeg` 返回 PCM16/PCM8-only `dsp_output_stream_pcm`，让 PCM DSP 输出能走 AudioUnit；FFmpeg 压缩格式完整恢复见 3.7.1。验证：安装/打开 `Final Battle, uid=0xA0003C62` 后显示语言选择界面（English / Ελληνικά / Русско / Deutsch / Italiano），日志无上述 DSP/KERN/access violation 错误。
5. **iOS 前端从不应用日志过滤器 + 非 `BUILD_FOR_USER` 默认 `*:trace` → 同步刷盘洪水把 CPU 打满**（N97/S60v5 点 Calculator 整屏黑 + CPU 100% 的**主因**）：从不调 `parse_filter_string` 叠加 `flush_on(debug)`，每次上下文切换/每条 VFP 子操作都同步刷盘（N97 启动 6s ~43 万行）；修法在 `IosEmulator` 镜像 `BUILD_FOR_USER` 降级应用 normal-use preset + 追加 `CPU*:warn`。**此修复只消除日志洪水导致的 100% CPU，N97 Calculator 仍黑屏**（剩余阻塞见下方已知风险 S60v5 FEP/Pti 项）。详见 [`ios-log-flood-cpu.md`](./ios-log-flood-cpu.md)。
6. **N95 Snakes 卡在启动画面 / `E32USER-CBase 46` 误信号**：**核心结论**——卡住是请求信号被误消耗和补发导致的调度失衡，修正请求等待与通知完成语义后，Snakes 可进入主菜单并实际游玩，Calculator 回归正常；详见 [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md)。
7. **N95 Snakes 真机黑屏无声（2026-06-03 已修）**：**核心结论**——iOS 真机构建从不把 HLE patch DLL 拷进 `data/patch`（旧 staging 只认 `__FILE__` 相对的开发机源码树、无 bundle 回退，仅模拟器侥幸成立）→ 零 patch → 无声 + Snakes 退回 `d_display.ldd` 触发活动调度器 panic → 黑屏。修法：把 `src/patch/*/group/*` 打进 .app `patch/`，staging 优先 bundle。真机验证声音+画面正常。详见 [`ios-device-missing-patch-dlls.md`](./ios-device-missing-patch-dlls.md)。
8. **主界面右上角 `+` 点击无反应**：主界面同时挂了 SIS 与 N-Gage 两个 `.fileImporter`，SwiftUI 在同一 view 上会丢掉其中一个 presentation，导致 `showingSisImporter = true` 后没有系统文件选择器。修法：引入 `HomeImportTarget`，SIS 与 N-Gage 共用一个 `.fileImporter`，按目标动态切换 `allowedContentTypes` / `allowsMultipleSelection` 并分发到原处理函数。验证：`./scripts/build_ios.sh simulator` 通过；booted iPhone 16 Pro / iOS 26.5 安装 Debug build，完成 onboarding 后通过 XcodeBuildMCP 点击 `Add`，截图确认 UIDocumentPicker 弹出到 `5320 (S60v3)` 文件夹。
9. **安装 X7（Symbian^3）ROM 后应用每次启动必闪退**：X7 图标是 NVG（Nokia 矢量图形）格式，含相对坐标路径段。NVG→SVG 解码器 `nvg_generate_direction` 用 `segment_type & ~1`（剥掉最低位的绝对/相对标志）查表命中命令，却用**带标志的原始值** `switch (segment_type)` 分发；相对段（奇数值）匹配不到任何 `case`（全为偶数）→ 落 `default: assert(false)` → `abort()`。图标在 applist 反复渲染故每次启动必崩。修法：`switch` 与弧线分支的 `==` 比较统一改用 `base_type = segment_type & ~1`；顺带修同函数内映射表把 `VG_SCCWARC_TO` 写重、漏了 `VG_SCWARC_TO` 的笔误。验证：模拟器安装 X7 后设备正常引导、applist 图标全部渲染、进程稳定 ≥20s、无 crash report。
10. **切换设备 / app 内重启（reboot）时 flexible 内存模型 teardown UAF**（TestFlight「反复闪退」的一支，`~flexible_mem_model_chunk → ~memory_object → decommit → mapping->owner_->id()` 段错误）：共享 chunk 的 `fixed_mapping_` 由 `do_create` attach 进 `mem_obj_->mappings_`，但（不同于 per-process mapping）从不 detach；成员逆序析构使 `fixed_mapping_` 先于 `mem_obj_` 销毁，`~memory_object` 的 `decommit` 遍历到已释放的 mapping。叠加 `control_flexible::~control_flexible()` 先销毁 `kern_addr_space_` 再销毁 chunk，使 mapping 的 owner 也悬垂。修法：① chunk 析构显式 `mem_obj_->detach_mapping(fixed_mapping_.get())`；② control 析构改为先 `chunk_mngr_.reset()` 再 `kern_addr_space_.reset()`。验证：运行中连续 4 次 5320↔X7 切换（各触发完整拆除+重建）零崩溃、进程存活、无新 crash report；5320 回归 8/8 PASS。
11. **切到 X7（Symbian^3）后打开 Calculator 整个应用卡死（黑屏 0 FPS，非崩溃）**：主线程 `viewDidAppear → launchAppWithUID → bind_graphics_driver → screen::set_screen_mode → create_bitmap → send_sync_command` 同步阻塞在图形工作线程；而图形线程处理排队的 `bind_bitmap(swapchain) → bind_swapchain_framebuf → context->update → gl_context_eagl::attach_layer` 时又 `dispatch_sync` 回主队列（CAEAGLLayer attach 是 iOS 26 硬性要求主线程，见 commit b9e92897b）。主等图形、图形等已阻塞的主队列 → 死锁。X7 切换改了分辨率使 `new_surface_size_` 处于 pending，故在 X7 稳定复现（通用线程缺陷，非 X7 专属）。修法：`launchAppWithUID` 改为在串行控制队列上异步执行（`launchAppWithUID:completion:`），让主 runloop 空闲以服务图形线程的 `dispatch_sync(main)`。验证：X7 打开 Calculator 正常渲染键盘、无死锁/panic、进程存活；5320 回归 8/8 PASS。
12. **Angry Birds（`0x20030E51`）在 X7 上安装与运行 + Full Screen 布局支持旋转横屏**：SISX 经 `onOpenURL → installSis` 装到 drive E，X7 上启动后从 LOADING 进入主菜单、原生 GLES shader 渲染、稳定 40 FPS、无 panic。横屏：`fullscreen` 键盘布局（触屏设备默认）此前被钉死竖屏，横屏 guest 只能在竖屏中间以 letterbox 显示。修法：给 `KeypadLayout` 增 `supportedOrientations`（fullscreen=`.allButUpsideDown`，classic/compact=`.portrait`，ngage=`.landscape`）+ `allowsFreeRotation`；fullscreen 走自由旋转（不强制，随物理设备）。**关键坑**：SwiftUI 的 `WindowGroup` 把每个页面托管在无法子类化的 `UIHostingController<…>` 里，其 `supportedInterfaceOrientations` 把窗口钉在 portrait（导航 push 后 requestGeometryUpdate 报 “Supported: portrait”，autorotation 同样被拒），即便 app delegate 已放开。修法：`routeOrientationThroughLiveControllers` 在运行时遍历活动控制器链，按类首次遇到时 method-swizzle `supportedInterfaceOrientations` 走全局 `lockedInterfaceOrientationMask`。验证：模拟器实测强制横屏后渲染视图 bounds 变 `874x402`（landscape=true），确认旋转管线打通、横屏全屏；模拟器 `simctl` 截图因外接显示器不反映旋转，物理旋转需真机确认。
13. **触屏 guest（fullscreen 布局，如 Angry Birds）真机触摸不灵敏 + 双指缩放无反应**：`EKA2L1RenderView` 常驻挂了 `UILongPressGestureRecognizer`（0.45s→Select 键）和 `UIPinchGestureRecognizer`（→上/下键）两个手势快捷方式。二者默认 `cancelsTouchesInView=true`：长按识别后取消传给 guest 的触摸序列（破坏弹弓「按住-拖拽-释放」瞄准），双指 pinch 吞掉第二根手指并转成单个按键（原生双指缩放到不了 guest）。多点触控在 bridge 层本已支持（每个 `UITouch` 映射独立 `mouse_id`）。修法：**直接移除这两个识别器及处理函数**（所有布局都不再拦截，原生多点触控直达 guest）；虚拟键盘本就有 d-pad/Select 键，这两个手势快捷方式冗余。验证：`simulator` 构建通过、5320 回归 8/8 PASS；真机触屏手感/双指缩放需在 iPhone Air 确认。
    - **待办（未修）**：X7 计算器**图标类功能键**（顶部一排 C/清除/退格等）渲染成结构化噪点（水平条带+亮条，疑似错误 stride/bpp 解释的位图），数字/运算符等**文本字形键正常**。与 applist 的 NVG 图标路径不同（applist 图标已正常），疑在 AVKON skin/MBM 位图→纹理上传管线；计算器功能可用，属独立较深的渲染问题，待后续单独排查。
14. **X7（Symbian^3）Angry Birds 手势"点一下加载画面就全废 / 双指缩放后全废"**：iOS 桥把 `pointerId = ObjectIdentifier(UITouch)`（对象地址）截断塞进 `mouse_id`，winserv 再原样填进 `adv_pointer_evt_.ptr_num`（`uint8_t`）→ guest 收到的 `TAdvancedPointerEvent::PointerNumber()` 是地址低 8 位（如 64/160/192 等垃圾值）。Symbian^3 游戏（AB 用 Marmalade/s3e）按指针号索引固定触点槽位，只接受 `0..N-1` 的小指针号：垃圾指针号的事件被静默丢弃或搞乱其触点状态机。地址是否"碰巧可用"取决于 UIKit 何时复用/新分配 `UITouch` 实例——加载/开场动画期间点一次或双指捏合都会引入新的 `UITouch` 地址，此后手势永久失效；模拟器上低字节恒为 0x40，事件全程送达（winserv 投递 + client 取走均正常）但游戏完全不响应，据此定位。S60v5 走老式 `TPointerEvent`（无指针号字段），故不受影响。修法（对齐 Qt 前端 `map_mouse_id_to_touch_index`）：`IosEmulator.submitPointerEventAtX:` 维护 `UITouch 地址 → 0..7 槽位` 映射，press 分配最小空闲槽、release 归还，未知指针的 release 丢弃，保证 guest 每个指针槽看到严格配对的 down/drag/up。验证：X7 + Angry Birds 模拟器实测——修复前主菜单 PLAY 点击完全无响应（ptr=64），修复后（ptr=0）PLAY/关卡选择/滑动翻页/弹弓拖拽全部正常，且加载画面上连点两次后手势依旧正常；5320 回归通过。真机双指捏合与加载点击需在 iPhone Air 复核。

15. **TestFlight「反复开不同 app 偶现闪退」：`ipc_msg` 引用计数破坏 → 线程对象 UAF / 堆污染**（同一 build 三份 .ips：reboot teardown 时 `wipeout → ipc_msg::unref` 段错误 + 两个主线程 UIKit 随机 `objc_release`/`CFRelease` 垃圾指针）：**核心结论**——`free_msg` 对仍被 server 持有的 sync 消息强制清零 ref_count，槽位被回收后迟到的 completion 偷减新属主线程的 access count，线程提前释放，悬垂 `own_thr` 的运行期写访问污染堆、teardown 强制 unref 直接崩。修复全部位于内核通用层（free_msg 跳过在引用消息、unref 下溢 guard + 摘 delivered 链 + 回收死属主 sync 槽、wipeout 中和残留消息、ipc_copy 引用泄漏）。验证：Release 回归 8/8 PASS + 连续 5 次交替启动两 app 无崩溃。详见 [`ios-ipc-msg-refcount-uaf.md`](./ios-ipc-msg-refcount-uaf.md)。

16. **X7（Symbian^3）Asphalt 6（`0x2003B2CC`）启动黑屏/卡死**：补齐 SCDV `EColor16MAP`/`MSurfaceId`、Belle executive/LDD、ROM E32 DLL、MMF/ALF、screen mode、Accessory 与 Loader 等通用缺口后，游戏可从主菜单进入 Nassau Normal Race 并正常渲染/驾驶；没有游戏 UID 特判。SCDV 的 ABI-preserving DLL 制作过程、逐阶段根因和 `ios_regression_test.sh asphalt6` 验收见 [`ios-asphalt6-x7.md`](./ios-asphalt6-x7.md)。

17. **Asphalt 6 真机早期黑屏 + host 偶现卡死（TestFlight `260737`）**：三份 `.ips` 经 GitHub Actions dSYM 精确符号化均为 `0x8BADF00D scene-update watchdog`；共同死锁是 guest/SVC 持 kernel lock 调 `AudioOutputUnitStop`，CoreAudio render callback 又阻塞等 kernel lock，主线程 touch/close 随后也被堵住。修法：实时音频 buffer 通知改为 kernel `try_lock`，锁忙时保留请求并在下一 callback 重试。另确认早期 Gameloft/Asphalt 两段 MP4 纯黑是 iOS bundle/staging 只收 `_general.dll`、遗漏 `mediaclientvideo_v100.dll`；改为携带全部版本化 patch DLL，由既有 loader 按 EPOC 版本选择。Release 验证：Asphalt 专用回归 **9/9**（含真实 Gameloft 电影帧与完整比赛）、标准 **8/8**、Angry Birds **5/5**；真机复测待 iPhone Air 在线。详见 [`ios-asphalt6-x7.md`](./ios-asphalt6-x7.md)。

18. **TestFlight `260729` CoreMotion worker 空指针（`.crash`）**：GitHub Actions run `29094599295` 的 dSYM UUID 与报告精确匹配，栈为 `desc_base::get_max_length → ipc_context::write_data_to_descriptor_argument → sensor_client_session::complete_channel_data_request → sensor_driver_ios::dispatch_sample`。根因是传感器完成在 kernel lock 外先移走 pending IPC，guest `StopListening`/close 可释放两个 client descriptor，host 再写 slot 2 counts package 时解引用空指针；callback 捕获裸 session 另有同源 UAF 窗口。修法：shared callback lifetime state + 完成入口先拿 kernel lock并校验 session/requester/descriptor，stop/close 同锁取消 pending request，iOS controller 取消时清 callback，IPC descriptor helpers 空指针防御。Release 验证：标准连续两轮 **8/8**、Asphalt **9/9**、Angry Birds **5/5**；真机 CoreMotion 复测待设备在线。详见 [`ios-sensor-async-ipc-crash.md`](./ios-sensor-async-ipc-crash.md)。

19. **Asphalt 6 dyncom VFP 热点与回归假阳性**：回归不再用 HDRI `AE` 把主菜单局部动画误判成页面跳转；先按 guest-band 亮度/方差确认交互标题，再以归一化 RMSE 验证各整页切换，最终比赛同时对比预览和已确认主菜单。性能侧为 VFPv3 单精度四种 MAC 增加 bit-exact packed host 快路径，Release 编译期移除 VFP trace 过滤调用，特殊值/模式仍走 softfloat。`dyncom_difftest` 20 万次 VFP soft/host A/B 零差异，microbenchmark 约 **1.45x**；Release 标准连续两轮 **8/8**、Angry Birds **5/5**、Asphalt **9/9**（另以 `EKA2L1_NO_VFP_HOST=1` 对照 9/9）。整机 FPS 仍受 simulator software GLES 限制，场景波动覆盖 CPU 小幅收益。详见 [`dyncom_optimization_plan.md`](./dyncom_optimization_plan.md)。

20. **Asphalt 6 主菜单 Mini 车身不可见（已解决）**：根因是 iOS 位图 FBO 无深度附件——`instantiate_bitmap_depth_stencil_texture` 的 iOS workaround 返回 `nullptr`，而 EGL window surface 同样以位图 FBO 作后备，深度测试恒通过，车身画出后被后续混合地板/光泽 pass 按画序覆盖。修复：iOS 改用 `depth24_stencil8` renderbuffer 做位图深度附件（当年只有*纹理*附件不完整），其它平台保持深度纹理路径。车漆恢复饱和蓝色且菜单 FPS 16→21。Release 标准回归 **8/8**、Asphalt **9/9**。详见 [`asphalt6-menu-car-material-investigation.md`](./asphalt6-menu-car-material-investigation.md)。

21. **5320 天地道固定横屏画面条纹**：游戏直接按 320×240 写 framebuffer，但没有像 Jelly Chase 一样向 Window Server 切换横屏，guest 仍以 240×320/错误 stride 上传。iOS 游戏菜单新增按 UID 保存的“旋转 Guest 画面”按钮：每次点击循环设备在 `wsini.ini` 中定义的真实 screen mode（5320 为竖屏 0°、横屏 90°、横屏 270°），让 Window Server/HAL/DSA/输入坐标同步收到模式变化；键盘布局与 guest 方向保持解耦。同时移除游戏菜单的 Restart Game 入口及相关逻辑。初版把 mode 存在 iOS `UserDefaults`、于 launch completion 一次性恢复，真机上会被稍后创建的 Window Server 窗口组重置回 mode 0；现改为复用 Android/Qt 的公共 `data/compat/<UID>.yml` per-app 配置，增加 `screen-mode` 字段并在对应窗口组获得焦点时恢复。验证：天地道 mode 2 正向无条纹，游戏菜单退出后从应用列表重进、以及宿主进程完全重启后均自动切回 mode 2；Jelly Chase 默认横屏路径不受影响；Release 标准回归连续有效轮次 **11/11**。

### 阶段 3 已知风险
- ✅ **mmap / mprotect 行为差异（3.1）已解决**：根因是 Apple Silicon 16 KB host page 与 4 KB `mprotect` 粒度不匹配（确定性、真机同样适用），按 host page size 对齐后消除。
- ✅ **cubeb iOS 后端历史包袱（3.7）已消除**：弃用 cubeb，改接原生 AURemoteIO / AVAudioSession（`audiounit_ios`），不再存在 cubeb 编译风险。
- **AVAudioSession 与 EAGL 生命周期耦合**：进后台须先 deactivate 音频再 pause GL，scenePhase 路径要写明先后顺序，否则 AudioUnit 可能在 EAGL context 释放后仍持引用而 crash。
- **Core Haptics 在模拟器不可用（平台限制）**：振动只能上真机验证，CI 烟测跳过振动相关 assert。
- **UIDocumentPicker 返回 security-scoped URL（3.5 进行中）**：拷贝前后必须配对 `startAccessingSecurityScopedResource` / `stop...`，否则读不到文件。
- **mem/chunk 改动须做桌面回归**：3.3 的 `virtualmem` 改动要在 macOS / Linux / Win32 桌面 Qt 上各跑一次，防 ABI 漂移。
- **S60v5 重 app 卡在 AVKON FEP / PtiEngine 启动（未解决，独立立项）**：N97 Calculator 黑屏是 backend / iOS 无关的通用 S60v5 重 app 启动缺口，验收改用 ZipManager 等轻量 app，详见 [`s60v5-avkon-fep-pti.md`](./s60v5-avkon-fep-pti.md)。
- **iOS 默认 dyncom vs sim dynarmic（待评估）**：sim 切 dynarmic 实测 N95 Calculator 完整渲染、未复现历史 regalloc crash，但因不解决 N97 且反转既有决定，留作后续单独评估。

---

## 阶段 4：dynarmic JIT + 发布通道 + CI

### 目标
把 dynarmic JIT 在 iOS 上启用起来，同时把 sideload / TrollStore / 越狱三套签名打包流程文档化；GitHub Actions 跑 iOS 构建烟测。
JIT 和发布通道绑在一起是因为各通道下能否拿到 JIT entitlement、用何种机制启用，是同一个工程问题的两个面。

### 验收标准（草稿）
- [ ] iOS 真机（dev signed + debugger / TrollStore / 越狱）上能切到 dynarmic 后端运行同一段 smoke blob，结果与 dyncom 一致。（构建/开关链路已就绪，待真机 + JIT 使能环境实测）
- [x] 无 JIT 权限的环境下，cpu factory 安全回落 dyncom，且 UI 明确告知用户原因。（`host_can_jit()` 运行时探测 RW→RX mprotect；设置页 footer 提示需 debugger/JIT enabler）
- [ ] `common::virtualmem` 提供统一的 `map_executable` / `jit_write_protect` 接口，dynarmic `block_of_code` 走该接口分配可执行内存。（暂不需要：dynarmic arm64 后端经 oaknut `CodeBlock` 已内置 `TARGET_OS_IPHONE` 的 RX 映射 + mprotect RW/RX 切换路径）
- [ ] 有 dyncom vs dynarmic 的简单 benchmark 数据登记在本文件。
- [x] CI 上 iOS 构建产物可下载。（`ios-unsigned-ipa.yml` 产 unsigned IPA（默认 JIT ON）、`ios-testflight.yml` 走 archive（JIT 永远 OFF））
- [ ] README 写清楚三种安装方式的步骤与 JIT 启用方法。

### 子任务
> 进入此阶段时再拆。
>
> 已落地：`EKA2L1_IOS_DYNARMIC` 统一开关（模拟器默认 ON、真机仅 unsigned/sideload 构建 ON，TestFlight/App Store 不编入任何 JIT 代码）+ 运行时 JIT 权限探测 + 设置页 dynarmic 开关（详见变更日志 2026-07-07）。遗留：dynarmic 的 A32 崩溃（Calculator SIGSEGV）未修，故 dyncom 仍是默认，JIT 为实验性 opt-in；真机 JIT-enabled 环境实测与 benchmark 未做。

---

## 阶段 5：系统能力对齐 Android

### 目标

盘点并补齐 iOS 相对 Android 前端缺失的平台系统能力（盘点日期 2026-07-10）。已确认对齐、无需动作的能力：传感器（两端均只有加速度计，通道模型一致）、振动、音频输出（AudioUnit vs cubeb）、FFmpeg 音视频解码、输入/是否对话框、截图、手柄、虚拟键盘、E 盘挂载。

### 子任务（按实施顺序）

#### 5.1 防休眠（wakelock 对标）✅（盘点误报，已存在）

- 盘点时大小写敏感的 grep（`idleTimer`）漏掉了 `isIdleTimerDisabled`：`EmulatorView.swift` 的 `onAppear`/`onDisappear` 早已成对设置/恢复 `UIApplication.shared.isIdleTimerDisabled`，模拟器界面期间不会锁屏。无需动作。

#### 5.2 相机后端（ECam / AVFoundation）🟡（已落地，待真机功能验证）

- 驱动层唯一整块缺失的后端：Android 有 `drivers/src/camera/backend/android/`（Camera2 取景器帧 + 拍照回调）+ `EmulatorCamera.java`；iOS 此前落到 `camera_collection_null`，拍照类应用与用相机的游戏无画面。
- 已落地：`drivers/{include,src}/.../camera/backend/ios/camera_ios.{h,mm}` —— `collection_ios`/`instance_ios` 对齐 Android 后端语义（索引 0=后置 1=前置、reserve 独占、7 种帧格式、尺寸阶梯对标 Camera2 列表形状）。取景器走 `AVCaptureVideoDataOutput`（BGRA→按 guest 请求缩放/转 FBS 16MU / 64K，行对齐同 Android）、拍照走 `AVCapturePhotoOutput`（JPEG/EXIF 直通或重缩放重编码；raw 格式经 CG 解码转换）；权限在 `ecam_create` 解锁内核期间信号量阻塞等待；回调采用拷贝后锁外调用，避免 Android 后端存在的 kern↔callback ABBA 死锁形。`Info.plist` 已加 `NSCameraUsageDescription`。
- 验证：模拟器无相机设备（count=0，行为同旧 null 后端），Release 回归 8/8；device（OS64）构建通过。**遗留**：真机上找一个用取景器/拍照的 guest 应用做功能验证（iPhone Air 当前不在线）。
- **相机取景下 Avkon 菜单被取景覆盖/闪烁**（真机 5320 Camera，左软键开菜单只闪一帧被盖）：取景窗口逐帧走客户端（局部）重绘，不清屏也不重新合成上层窗口，导致压在取景之上的菜单面板区域被越界的取景像素盖住且不被重画覆盖。修法：`canvas_base::try_update` 中，当重绘窗口被部分遮挡（`visible_region` 非空且非自身整块）时升级为一次服务端整屏合成，让上层窗口按后到前重画在最上。共享 window server 修复，非相机特判。详见 [ios-camera-menu-over-viewfinder.md](ios-camera-menu-over-viewfinder.md)。验证：真机菜单稳定显示；Release 回归 8/8 + Angry Birds 触屏 5/5。
- **相机 guest 内 Exit 退出时整个 app 卡死（10s 看门狗 `0x8BADF00D`）**（真机 5320 Camera）：退相机时 guest 线程在**持有内核锁**的 `ecam_release` HLE 里，用 `dispatch_sync` 同步调 AVFoundation `AVCaptureSession -stopRunning`（阻塞、内部转 runloop 且需主线程）；内核锁被长时间攥住，主线程（正处理软键释放 `submitRawKey→窗口服务→kern->lock()`）与 Timing 线程都卡在等锁 → 真死锁被看门狗杀掉。修法（对齐 `ecam_create` 早有的"相机阻塞操作不能攥内核锁"惯例）：`camera_ios.mm` 的会话拆除 `-stopViewfinder`/`-shutdown` 由 `dispatch_sync` 改 `dispatch_async`，guest 线程不再阻塞、内核锁立即释放；帧回调在调用前已置空，异步停不漏帧。验证：真机 guest 内 Exit 不再卡死。
- **宿主菜单退出游戏崩溃（`EXC_BAD_ACCESS` 悬垂指针）**（真机 5320 Camera）：宿主菜单 `closeRunningApp→pr->kill()` 直接杀 guest 进程、不走 `ecam_release`，AVFoundation 仍在出帧；进程被杀后视频队列上迟到的取景回调仍去 `done_frame_viewfinder_notify_.complete()`，解引用已销毁的 `requester` 线程 → 段错误。与音频后端已解决的同类 UAF 一致（`audio.cpp` `complete_audio_notify_if_alive`）。修法：`camera.cpp` 加 `complete_camera_notify_if_alive`，在内核锁下用 `kern->is_thread_alive(requester)`（只比对活线程表、不解引用）校验，活着才 `complete()`、否则丢弃过期请求；用于取景帧与拍照完成两个异步回调。验证：真机宿主菜单退出不再崩溃。

#### 5.3 系统语言设置 ✅

- Android 经 `getLanguageIds` / `getLanguageNames` / `setLanguage` 暴露 guest 系统语言；iOS 此前无任何入口，用户只能手改 `config.yml`。
- 已落地：桥接层新增 `availableLanguages` / `currentLanguageCode` / `setSystemLanguageCode:`（对齐 Android `set_language_current`：写 config + `set_system_language` + 重写 locale 内核属性），`bootDeviceAtIndex:` 增设备切换语言回退（配置语言不在设备 ROM 语言表时退回设备默认，对齐 Android `set_current_device`），SettingsView 设备区新增 System language 选择器。
- **顺带修复跨平台缺陷**：`applist_server::app_language` 此前硬编码返回英语——AVKON 通过 apparc 的该查询决定加载哪份 `.rXX` 翻译，导致即使内核语言已切换、ROM 也带翻译，应用 UI 仍是英文；改为返回 `kern->get_current_language()`（非法/any 回退英语），三平台同收益。
- 验证（模拟器 N95/rm-320，Release）：设置页列出 ROM 全部 6 语言，选 French 后 `config.yml` `language: 2`，重启进 Calculator 软键变 “Quitter”（calcsoft.r02 生效）；切回英语后回归 **8/8**。

#### 5.4 蓝牙联机（BT Netplay）🟡（已落地，待真机双机联测）

- 此前三处断链：① `miniupnpc` 在 iOS 被排除构建（`upnp.cpp` no-op，任务 0.5 遗留）；② 前端无配置 UI；③ `Info.plist` 无本地网络权限声明。
- 已落地：① miniupnpc 纳入 iOS 构建（模拟器 + OS64 真机均编译通过），`upnp.cpp` 移除 iOS stub 走真实 Dolphin 移植实现；② SettingsView 新增 Bluetooth Netplay 区块（发现模式 Off/Direct IP/Local network/Central server、端口偏移、密码、UPnP 开关、Direct IP 好友地址增删），config snapshot 桥接 `btnet_*`/`enable_upnp`/`friend_addresses`；③ `Info.plist` 补 `NSLocalNetworkUsageDescription`。midman 在设备 boot 时读配置，改动自下次 app 启动生效（UI footer 已注明）。
- 验证：Release 回归 8/8；UI 实测切 Direct IP → `btnet-discovery-mode: 1` 写盘、添加 192.168.1.50:35689 → `internet-bluetooth-friends` 正确序列化。**遗留**：真机双设备实际联机对战（如 N-Gage 双人游戏）未测；`btnet_password` / 中央服务器路径未实测。

### 待排期（暂不实施，视反馈提升优先级）

- **每应用设置 UI**：核心 `config::app_settings` 已实例化（手放配置文件会生效），但无编辑入口；Android 经 `updateAppSetting` + AppDataStore 暴露 per-app FPS / time delay / 屏幕旋转 / upscale / filter shader / child-inherit。
- **其余系统配置项 UI**：IMEI、MMC ID、RTOS level、MIDI 后端选择（tsf/minibae）、屏幕缓冲同步。
- **SIS 包管理 + N-Gage 2.0 license**：Android 有 `getPackages` / `uninstallPackage`（SIS 注册包粒度）与 `installNG2Licenses` / `getFailedInstalledLicenseGames`；iOS 只有按 app UID 卸载与经典 N-Gage 安装。
- **miniBAE HSB 音色库**：iOS 只 stage 了 `defaultbank.sf2`，切 minibae 后端会缺 `defaultbank.hsb`（默认 tsf 不受影响）。

---

## 变更日志

| 日期 | 改动 |
|------|------|
| 2026-07-15 | **修复 6680(rm-36) 等 S60v2 设备系统图标反相**：上一提交(118dcfa8)按 has-colour 标志区分遮罩极性，把 S60v2 ROM 图标的 **color256(bpp8,color=1) 灰度软遮罩**误判为彩色 color-key 遮罩并反相 → 前景被抠成透明、背景变不透明，整屏系统图标反相破损。实测两代设备遮罩：S60v2 系统图标遮罩为 color256/gray256、白=前景(不透明)，兄弟连(BIA3D) 遮罩为 **color64k(bpp12/16)**、白=背景(需反相)，二者内容都是灰度值(colorful=0)，靠 has-colour 标志无法区分。改判据为**位深**：真正的 alpha/软遮罩都是 ≤8bpp 调色板灰度(gray2/4/16/256 + 偶发 color256)，直接取亮度(红通道)作 alpha(白=不透明)；≥12bpp 彩色位图才是被复用的 color-key 遮罩，沿用 make_standard_mask 的纯白判定反相(白=透明)。color-key 分支字节等价于原提交(BIA 无回归)。验证：6680 全部系统图标恢复正常彩色+透明，5320 上兄弟连(色照)及 Final Battle/七夜/天地道 等无回归；Release 标准回归 **11/11**。 |
| 2026-07-15 | **修复 5320(rm-409) 三款 S60v3 游戏图标异常**：兄弟连(BIA3D.mbm) 图标背景发紫红、七夜(7Days.mif) 与天地道(HellStrikerII.mif) 图标不显示。定位为两个独立 bug：① iOS 图标解码器 (`decode_mbm_icon`/`decode_bitwise_icon`) 从不应用配对遮罩(mask)，被遮罩的品红 color-key 背景整块不透明。补上 mask 合成，且极性按 mask 类型区分——灰度遮罩(gray2/gray256)值即 alpha(白=不透明)，彩色 color-key 遮罩(经 make_standard_mask)则白=透明；否则 Final Battle 的 gray256 遮罩会被反相抠掉图标露出品红背景。② 七夜/天地道 图标实为 SVG 内嵌 base64 PNG(`<image xlink:href="data:...base64,...">`)，旧 lunasvg 2.3.8 fork 不支持 `<image>`/data-URI。**将 EKA2L1/lunasvg fork 合入上游 v3.5.0**(原生 `<image>`+plutovg，项目仍依赖该 fork)。上游 3.5.0 是完整重写、不含 fork 早年 `<switch>` stub(e1c60a5)，而 Adobe 导出的图标把 `<image>` 包在 `<switch><foreignObject requiredExtensions=.../>` 内，lunasvg 遇未知元素会跳过整棵子树 → 图标空白；因此在 fork 里按 3.5.0 代码结构**重新实现 `<switch>` 元素**(解析为 group，`<foreignObject>` 在解析阶段被丢弃，剩下的即可绘制子节点)，本体侧无需 workaround(fork 的其余历史改动仅 C++11 兼容 shim 与已并入上游的 move ctor，对 3.5.0/本工程 C++20 不适用)。验证：5320 上三款游戏图标全部正常，Final Battle 等对照图标无回归；Release 标准回归 **11/11**。 |
| 2026-07-14 | **修复游戏前后台切换后永久静音**：后台时 AVAudioSession 被主动停用，但重新激活并不保证已有 AURemoteIO 自动恢复 render callback；因此游戏仍运行，而退出重进通过重建 stream 才恢复。iOS 音频 driver 现登记存活 stream，生命周期 pause 标记 driver suspended，前台成功激活 session 后、guest 恢复执行前显式 Stop/Start 原本正在运行的 AudioUnit；登记锁同时阻止 stream 析构与恢复并发，保留 guest 侧播放状态。Debug/Release 模拟器构建通过；Release 标准回归连续两轮 **11/11**；Final Battle 游戏内实际 Home→点图标回前台后仍停在同一 guest 场景，等待 20 秒后画面正常，日志无 AudioUnit/AVAudioSession 错误、guest panic 或 host crash。模拟器无法确认扬声器听感，仍需真机复听。 |
| 2026-07-14 | **移除 iOS 位图深度附件特化（统一回深度纹理路径）**：DEPTHPROBE 实测当前 iOS 模拟器 GLES 完全接受 depth24-stencil8 纹理附件（合并/分离 attach 均 `GL_FRAMEBUFFER_COMPLETE`、零 GL 错误），当年 `f1452b608` 判定"iOS 拒绝深度纹理附件"应为同笔提交一起修复的 BGRA internal format 颜色附件失败被误归因。删除 renderbuffer 分支与 `bitmap::ds_tex` 的 drawable 放宽，iOS 与 Android/桌面统一走上游深度纹理路径（该区域相对上游零 diff）。验证：Asphalt 6 车漆正常、Release 标准回归 **11/11**（含 N95 boot——正是当年引入 hack 的场景）。 |
| 2026-07-14 | **修复 N95 启动堆损坏（E32 镜像 inflate 尾字越界读）+ 新增 n95calc 回归用例**：rm-320 启动 Calculator 稳定崩溃且崩溃栈随机游走（FreeType double-free、`font_atlas::draw_text` 越界 memmove、`update_bitmap` free 非法指针、window server 野指针），基线版本同样复现，是遗留问题而非当日改动引入。ASan 一击定位：`flate::bit_input::read(int)` 按 32 位字预取，boot 加载 patch DLL 解压 E32 镜像时压缩流长度非 4 的倍数，最后一个字越界读入**随机堆内容**，解压出的 patch DLL 代码逐次不同 → guest 行为随机 → host 堆被写坏。修法：尾部不足一字时按剩余有效字节逐字节拼装（缺失字节为 0，与 `swap_bo` 全字读语义一致且确定），修复后 ASan 全程干净、非 ASan 连续 3 次启动存活。新增 `n95calc` 回归用例（并入默认 `all` 套件）：rm-320 启动 Calculator，断言 **host 进程存活**（查 DiagnosticReports 新增 .ips + launchctl 进程在列）、UI 非空白、guest 无 panic——host 崩溃是既有套件从未覆盖的失败模式。Release 标准回归 **11/11**（FBattle 3 + Calculator 5 + N95Calc 3）。 |
| 2026-07-14 | **iOS GL 后端加固（BGRA 上传 + base-vertex 警告）**：审计 iOS GL shim 后修复两处潜在偏差：① guest 经 `GL_IMG/EXT_texture_format_BGRA8888` 扩展上传 BGRA 纹理时，driver 把 BGRA 同时用作 internal format——这是 EXT 语义（Android/桌面正确），Apple 的 `GL_APPLE_texture_format_BGRA8888` 要求 internal=RGBA，否则报错留黑纹理；iOS 分支现将 internal 归一为 RGBA（尚无已知使用 BGRA 的测试游戏，待遇到时验证）。② `ios_gl_loader` 的 `glDrawElementsBaseVertex` stub 会丢弃 base vertex（GLES3.0 无此调用），当前所有调用点都传 0，但一旦出现非零会静默画错——现补一次性 LOG_ERROR。审计确认其余裁剪（PVRTC/ETC2 软解兜底、fb 读回格式兜底、1D 纹理 stub、CLAMP_TO_BORDER）均有护栏或无调用方。Release 标准回归 **8/8**。 |
| 2026-07-14 | **修复 Asphalt 6 主菜单车身不可见（iOS 位图 FBO 缺深度缓冲）**：动态取证证明车身 draw（`TEXTURED+MULTITEXTURED`，4449 索引）本身渲染完全正确（蓝色已画出），随后被本应被深度挡掉的混合地板/光泽 pass 覆盖；`glGetFramebufferAttachmentParameteriv` 显示场景 FBO `GL_DEPTH_ATTACHMENT=GL_NONE`。根因是 `graphics_driver_shared.cpp` 的 iOS workaround 让所有位图 FBO 无深度附件，而 EGL window surface 也是位图。修复：iOS 用 `depth24_stencil8` **renderbuffer** 替代被 iOS GLES 拒绝的深度纹理附件（`bitmap::ds_tex` 放宽为 `drawable`），Android/桌面路径不变。同时证伪了 attribute 错位、纹理 V 翻转、GL 字符串检测、dyncom 误算等旧假设（1716 draw 是深色 trim，本来就该黑）。Release 标准 **8/8**、Asphalt **9/9**，菜单 FPS 16→21。详见 [`asphalt6-menu-car-material-investigation.md`](./asphalt6-menu-car-material-investigation.md)。 |
| 2026-07-13 | **修复 build 260742 视频完成通知 UAF**：解码线程 `on_play_done` 不持 kernel lock 直接 `notify_info::complete`，requester 线程已随 app/播放器 teardown 销毁，`get_raw_pointer` 得到空 process（0x48）。修法沿用 audio/camera 的 complete-if-alive 模式：持 kern lock + `is_thread_alive` 校验，死 requester 丢弃通知；同时 play/stop/close/destroy 等会 join 解码线程的 bridge 调用先释放 kern lock 再操作（camera-create 先例），避免与完成回调抢锁死锁。Release 标准 **8/8**、Asphalt **9/9**。 |
| 2026-07-13 | **修复两份真机 crash（视频贴窗 UAF + timer 回调 UAF）**：① build 260741 `post_new_image` 崩溃（0xc0=空 `scr` 的 `screen_mutex`）——`postings_` 持裸 `canvas_base*`，窗口被 winserv 销毁后解码线程仍向其投帧，且 guest 线程无锁并发改容器；修法沿用 EGL surface 惯例：`epoc_video_player` 实现 `canvas_observer`，窗口死亡在新增 `postings_lock_` 下置空 posting（post 全程持锁），析构先 join 解码线程再摘 observer。② build 260738 Timing thread `timer_callback` 崩溃（0x78）——`ntimer::unschedule_event` 只删队列、追不回已弹出执行中的回调，guest 持 kern lock 销毁 timer 后回调解引用已释放 `signal_info`；修法：事件 userdata 由裸指针改为 timer 的 kernel uid，回调持 kern lock 用 `get_by_id` 重新解析，死 timer 安全空操作（unschedule 加屏障不可行：回调自身要拿 kern lock，会与销毁方死锁）。Release 验证：标准 **8/8**（含 FBattle timer-defer 路径）、Asphalt **9/9**。 |
| 2026-07-13 | **修复片头视频音频断续（Asphalt 6 开场电影"一顿一顿"）**：`video_player_ffmpeg` 的解码线程按视频帧率 sleep 节流 `av_read_frame`，音频包只是顺带入队，稳态余量不足一个视频帧周期（A6 片头 25fps=40ms vs AAC 包 21ms），任何一次迭代超帧期（sleep 超时、sws_scale、贴图上传、屏幕锁竞争）都会抽干音频队列，AudioUnit 回调补零产生周期性断音；进游戏不走视频路径所以正常。修法（通用，非平台/游戏特判）：读包与视频显示节拍解耦——视频包进有界预读队列（64 包），持续读到解码音频缓冲 ≥0.5s；AAC 解码/重采样移出实时音频回调、改在解码线程灌入无锁 SPSC ring（回调只 memcpy，实时安全）；EOF 完成回调等预读音视频排空后再发（避免读提前导致截尾）；顺带修正 `av_read_frame` 后包引用释放、`swr_convert` 按实际输出样本数入 ring、音频流创建失败时 `set_volume` 空指针。Release 验证：Asphalt 专用回归 **9/9**（含两段片头电影完整播放）、标准 **8/8**；真机听感复测待设备。 |
| 2026-07-13 | **修复重力感应轴向不随横屏适配（Asphalt 6 手感异常）**：CoreMotion 报的是 iPhone 机身固定坐标系，此前样本直达 guest，只有把 iPhone 按虚拟诺基亚的持机方向拿着才正确——真机标定（Asphalt 6 @ rm-707，横屏模式 `SCR_ROTATION2=90`）：逆时针横屏正确、顺时针横屏反 180°、竖屏偏 90°。修法（通用，非游戏特判）：`sensor_driver` 基类增 `set_motion_rotation(deg)` 默认空实现；iOS 后端在 `dispatch_sample` 对 X/Y 施加 `R(-θ)`；桥接每次 present 以 `guest screen_mode.rotation + host 界面方向角`（portrait 0 / landscapeLeft 90 / upsideDown 180 / landscapeRight 270）刷新 θ，自动跟踪 guest 模式切换与 host 旋转（旋转会触发 surface re-attach → re-present）。Android 后端同病（`SENSOR_LANDSCAPE` 原样透传），留待后续。Release 标准回归 **8/8**、Angry Birds **5/5**；真机三方向手感验证待 TestFlight 构建。 |
| 2026-07-13 | **收紧 Asphalt 6 回归并优化 dyncom VFP 热点**：用交互标题的 guest-band 统计 + 归一化 RMSE 消除片头延迟/主菜单动画假阳性；VFPv3 单精度四种 MAC 增加 bit-exact packed host 快路径，Release 编译期裁掉 VFP trace。CPU 差分 20 万次 MAC A/B 零差异，microbenchmark 约 1.45x；Release 标准连续两轮 8/8、Angry Birds 5/5、Asphalt 9/9，关闭 host 快路径对照仍 9/9。 |
| 2026-07-13 | **修复 TestFlight `260729` CoreMotion/Sensor Framework 异步写回崩溃**：CI run `29094599295` dSYM UUID 精确匹配 `.crash`，确认 worker 在 guest stop/close 释放 slot 2 counts descriptor 后进入 `desc_base::get_max_length(nullptr)`。完成路径改为 shared session lifetime state，先取得 kernel lock再访问 pending IPC，并校验 requester/descriptor；stop/close 取消请求，iOS controller 清理未派发 callback，通用 descriptor helper 拒绝空 guest 指针。Release：标准连续两轮 **8/8**、Asphalt **9/9**、Angry Birds **5/5**；真机 CoreMotion 复测待设备在线。详见 [`ios-sensor-async-ipc-crash.md`](./ios-sensor-async-ipc-crash.md)。 |
| 2026-07-13 | **修复 Asphalt 6 真机 watchdog 卡死与早期开场纯黑**：GitHub Actions 拉取 TestFlight `260737` 的匹配 dSYM，三份 `.ips` 精确收敛到同一 kernel↔CoreAudio 锁序环；buffer 通知改用 `try_lock` + 后续 callback 重试，消除 `AudioOutputUnitStop` 在 guest 持 kernel lock 时反等 render callback。早期片头另因 iOS 打包仅收 `_general.dll`、漏掉已有 `mediaclientvideo_v100.dll`，改为 bundle/staging 全部版本化 patch DLL。新增 Asphalt 回归断言真实 Gameloft 电影帧并等待两段电影完整结束。Release：Asphalt **9/9**、标准 **8/8**、Angry Birds **5/5**；真机复测待设备在线。详见 [`ios-asphalt6-x7.md`](./ios-asphalt6-x7.md)。 |
| 2026-07-13 | **打通 X7 Asphalt 6 到实际比赛**：补齐 SCDV `EColor16MAP` + `MSurfaceId`/旋转、Belle executive/LDD、ROM 非 XIP E32 与无盘符 rooted DLL 查找、MMF A3F/ALF、screen mode、Accessory modern IPC、Loader PDD 等通用兼容缺口；无游戏 UID 特判。新增可复现的 ABI-preserving SCDV 制作脚本与 `ios_regression_test.sh asphalt6` 回归。Release 实测进入 Nassau Normal Race；标准回归连续两轮 **8/8**，Angry Birds **5/5**。详见 [`ios-asphalt6-x7.md`](./ios-asphalt6-x7.md)。 |
| 2026-07-11 | **修复 5320 内置 Camera 真机 `E32USER-CBase 49`**：根因链包含三个通用缺陷。①ROM patch 的 `SVC #0xFF` trampoline 从未同步的 saved thread context 读写 PC，导致 lookup miss 且跳过 ecam patch；改为基于 live CPU core 按 ARM/Thumb 宽度还原 SVC 地址并直接跳转。②补齐缺失的 `ECamDuplicate`（dispatch `0x71`），共享 camera 实例/handle 并返回缓存 info。③iOS camera 声明 `CAPTURE_OPTION_ALL` 却拒绝部分标量参数，guest `Leave` 后在旧 ROM 低地址 patch DLL 展开路径中违规访问；为尚无原生映射的 optical zoom/contrast/brightness/white balance 提供实例级缓存 get/set。清理诊断代码后的最终真机包运行 Camera 约 55 秒，日志无 panic、trampoline miss、dispatch 113 缺失、unsupported parameter 或 access violation；Release 模拟器回归连续两轮 **8/8**。详见 [`ios-5320-camera-panic49.md`](./ios-5320-camera-panic49.md)。 |
| 2026-07-11 | **修复 `codeseg::attach()` 重新 attach 时的 codeseg 泄漏（跨平台，非崩溃）**：codedump 收集器 `add(codeseg*)` 把无 attach 的 codeseg 收进 decease 列表时 `increase_access_count()` 持有引用，`clean_impl()` 出队有配对 decrease，但 `attach()` 复用路径调的 `remove(codeseg*)` 只出队不放引用 → 被重新 attach 过的 codeseg 计数永远 ≥1、不再释放。修法：`remove(codeseg*)` 末尾补 `decrease_access_count()`（放在读 `get_used_memory_size()` 之后的最后一句，避免销毁后访问）。时序安全：唯一调用点 `attach()` 在 remove 之前已 `increase_access_count()` 持有自身引用，进 remove 时计数 ≥2，不可能在 attach 中途销毁；decease 列表成员必持收集器引用的不变量也保证列表内对象不会被旁路销毁。验证：Release 回归 **8/8**，日志无 panic/违规访问。 |
| 2026-07-11 | **修复 5320 File manager 秒退（跨平台内核缺陷：`Dll::FileName` 无法解析 XIP 静态链接 DLL）**：症状是点开 File manager 后 EmulatorView 立即弹回列表且无 panic。根因链：FileManager 的 GFLM 引擎用 `Dll::FileName` 取自身 DLL 路径来定位资源盘符 → `dll_filename` 只查 codeseg，而 XIP ROM 静态链接 DLL（gflm.dll 等）仅在出现于镜像 attach 链时才建 codeseg → 查找失败返回空文件名 → guest 回退 C: 盘拼出 `C:\resource\gflmfileextensionmimetypes.rsc` → 打开失败 Leave(-12/-1) → 进程 `User::Exit(0)` 干净退出（无 panic 所以无弹窗）。修复（`svc.cpp`，共享内核代码，Qt/Android 同收益）：`get_dll_full_path` 在 codeseg 未命中时回退遍历 ROM burn tree，按 XIP 条目 `address_lin + size` 范围反查所属镜像（`0x80e0c818` → `Z:\Sys\Bin\GFLM.dll`）；`lib_manager::get_drive_rom()` 提为 public。验证（5320 模拟器 Release）：File manager 打开显示 C/E 盘并可进入 C 盘目录浏览；回归 **8/8**。诊断技巧备忘：iOS 前端默认把日志降级为 normal preset，`config.yml` 设 `extensive-logging: true` 才能看到 `[Kernel]` trace（SVC/IPC/线程退出）。 |
| 2026-07-11 | **修复 TestFlight 26.7.0 (260734) 两例崩溃（音频析构纯虚调用 + codedump 收集器跨重启悬空链表）**。①`SIGABRT`（`__cxa_pure_virtual` @ AURemoteIO::IOThread）：app 退出时 `~keysound_session` 销毁 `audiounit_ios_output_stream`，`AudioOutputUnitStop` 原在**基类** `audiounit_ios_stream_base` 析构里执行——彼时派生类已析构、vtable 回退，飞行中的渲染回调调虚函数 `should_idle()` 撞纯虚。修法：新增幂等 `dispose_unit()`（Stop/Uninitialize/Dispose），改在最派生类析构调用（Stop 同步等回调收尾，窗口期对象完整），基类析构保留兜底。②`SIGSEGV`（`clean_impl → free_attached_data` 读野地址）：`codedump_collector_` 是 `kernel_system` 成员、跨 `wipeout()` 存活，但 wipeout 销毁全部 codeseg（侵入式 `garbage_link` 节点嵌在被释放的 `attached_info`/codeseg 里）时从不清收集器的两条链表，容量计数也不归零；内核重启（切/重开 app）后首次 `add()` 触发 `clean_impl()` 遍历上一世代悬空节点即崩，对应用户反复开关 app 场景。修法：`roundabout::reset()`（不触碰已释放节点、直接重置环头）+ `codedump_collector::wipe()`，在 `wipeout()` 销毁 codesegs 之后调用。两例均经 dSYM（CI artifact，UUID 匹配）符号化定位。验证：Release `ios_regression_test.sh` **8/8**。遗留：`codeseg::attach()` 从 decease 列表移除时未配对 `decrease_access_count()`，被重新 attach 过的 codeseg 泄漏（非崩溃，未改）。 |
| 2026-07-11 | **打通 iOS 蓝牙联机（BT Netplay，阶段 5.4）**：miniupnpc 纳入 iOS 构建（去掉 `src/external`/`common` 两处 `NOT EKA2L1_IOS` 守卫，模拟器 + OS64 真机编译通过），`common/upnp.cpp` 删除 iOS no-op stub 启用真实 UPnP 端口映射；`Info.plist` 加 `NSLocalNetworkUsageDescription`；SettingsView 新增 Bluetooth Netplay 区块（发现模式四选、端口偏移、密码、UPnP、Direct IP 好友地址增删——好友地址用 addr+port 字典桥接以兼容 IPv6 冒号），config snapshot 桥接全部 `btnet_*` 字段。midman 在 boot 构建时读配置，改动自下次 app 启动生效。验证：Release 回归 **8/8**；UI 实测发现模式写盘 `btnet-discovery-mode: 1`、好友地址序列化进 `internet-bluetooth-friends`。遗留：真机双机联机对战未实测。 |
| 2026-07-10 | **iOS 系统语言设置（阶段 5.3）+ 修复 applist AppLanguage 硬编码英语的跨平台缺陷**：桥接层新增 `availableLanguages`/`currentLanguageCode`/`setSystemLanguageCode:`（写 config + `set_system_language` + 重写 locale 内核属性，对齐 Android `set_language_current`），`bootDeviceAtIndex:` 在配置语言不属于目标设备 ROM 语言表时回退设备默认（对齐 Android `set_current_device`），SettingsView 设备区加 System language 选择器。关键根因：仅切内核语言后 guest UI 仍英文——`applist_server::app_language` stub 硬编码 ELangEnglish，而 AVKON 用它决定加载哪份 `.rXX` 翻译；改为返回 `kern->get_current_language()`（非法/any 回退英语），Qt/Android/iOS 同收益。验证（N95 模拟器 Release）：语言表列全 6 项、选 French 持久化 `language: 2`、Calculator 软键变 “Quitter”；回归 **8/8**。 |
| 2026-07-10 | **接入 iOS 相机后端（AVFoundation，对标 Android，阶段 5.2）**：新增 `drivers/{include,src}/.../camera/backend/ios/camera_ios.{h,mm}`，`camera_collection.cpp` 工厂增 iOS 分支，drivers CMake 补 CoreMedia/CoreVideo/CoreGraphics/ImageIO，`Info.plist` 加 `NSCameraUsageDescription`。语义对齐 Android：索引 0=后置 1=前置（各至多一个）、reserve 独占、同一组 7 种帧格式与 `CAPTURE_OPTION_ALL`；取景器 `AVCaptureVideoDataOutput` BGRA→缩放→FBS 16MU/64K（行对齐同 Android），拍照 `AVCapturePhotoOutput` JPEG 直通/重编码或 CG 解码转 raw；相机权限在 `ecam_create` 已解锁内核的窗口内以信号量阻塞请求。回调设计沿用 sensor_ios 的拷贝后锁外调用，规避 Android 后端的 kern↔callback ABBA 死锁形。验证：模拟器 Release 回归 **8/8**（模拟器无相机设备，行为不变）、unsigned device 构建通过；真机 guest 相机应用功能验证待 iPhone Air 在线后补。 |
| 2026-07-10 | **修复退出游戏后切设备闪退第二例（EGL surface 悬垂窗口指针，R8）**：`egl_surface` 持背靠窗口裸指针并注册 canvas observer，但 `~canvas_base` 从不通知/清空 observers——游戏退出销毁窗口后，未 `eglDestroySurface` 的残留 surface 在切设备拆 dispatcher 时 `remove_canvas_observer` 解引用已释放窗口（野地址 SIGSEGV，与 `.ips` 吻合；运行期 dead_pending 清理同样可触发）。修复=观察者双向解绑：`canvas_observer` 新增 `on_window_destroyed()`，`~canvas_base` 弹出并通知，`egl_surface` 置空指针（运行期使用点本就判空），跨平台生效。验证：Release 回归 **8/8** + AB（GLES 路径）**5/5**。详见 [docs/ios-testflight-crash-triage-r2.md](./ios-testflight-crash-triage-r2.md) R8。 |
| 2026-07-10 | **接入 iOS 传感器后端（CoreMotion，对标 Android）**：新增 `drivers/src/sensor/backend/ios/sensor_ios.{h,mm}`——`CMMotionManager` 加速度计，通道模型与 Android 后端一致（Nokia 5800 标定：scaled S8 ±127、量程 {19.62, 78.48} m/s²、采样率 {10,40,50} Hz），坐标系换算 = 取负 × 9.80665（iOS 报 g 且重力为负轴，Symbian/Android 报反作用力 m/s²）。工厂 `sensor_driver::instantiate()` 增 iOS 分支；前端 boot 时 `set_sensor_driver`、前后台随 `pause`/`resume` 启停、`bootDeviceAtIndex:` 重建前 pause 兼作在途回调 barrier（RAII 恢复）。锁序设计：回调收集与调用分离，避免 Android 后端存在的 kern↔sensor ABBA 死锁形；`sensor_driver` 基类补虚析构（跨平台 UB 修复）。模拟器无加速度计（查询回 0 通道，等价旧行为）；验证：Release 回归 **8/8** + AB 套件 **5/5**，真机功能验证待重力感应游戏确认。 |
| 2026-07-10 | **修复退出游戏后切设备偶现闪退（音频渲染线程 vs 拆除顺序，R7）**：`~dsp_epoc_stream` 只做虚拟 stop，成员逆序析构令 `lock_` 先于真正停硬件的 `ll_stream_` 死亡，窗口期渲染回调锁已析构 mutex → 异常穿过 CoreAudio C 回调边界 → `std::terminate`（`.ips` 里渲染线程 SIGABRT + switchDevice 线程卡 `AURemoteIO::Stop` 完全吻合）。修复原则=先同步停硬件流再析构回调可达状态（不是 try/catch）：`~dsp_epoc_stream` 显式 `ll_stream_.reset()`；同族审计另修 `~dsp_epoc_player`（unique_ptr 先置空后跑 deleter 的 null 窗口）、`~player_tsf`（先 `tsf_close` 后停流 = 渲染期写已释放 synth，堆损坏来源）、`~player_ffmpeg`（deinit 先于基类停流 + vtable 回滚纯虚）。验证：Release `ios_regression_test.sh` **8/8**（FBattle 音频 + 退出重建路径）。详见 [docs/ios-testflight-crash-triage-r2.md](./ios-testflight-crash-triage-r2.md) R7。 |
| 2026-07-10 | **TestFlight 崩溃第二轮归因与修复（6 个根因全修）**：`crashes/` 新导出 5 个崩溃点拆栈后收敛为——①`~system_impl` 期间 ntimer 线程仍跑定时器/动画回调（新增 `ntimer::stop()` 析构首步 join，顺带解释第一轮 B6 的 FreeType 堆损坏）；②`kernel_system::wipeout()` 先毁 chunks 后毁 processes 致 `~flexible_mem_model_process` UAF（重排顺序）；③codedump 收集器持有已 erase 的 attached_info（`free_attached_data` 先 `collector.remove`，即「退出游戏偶崩」`DVNi…` 的主因）；④父子进程裸指针从不解链（`process::kill` 里解链）；⑤位图哈希用 guest 可控 `bitmap_size` 越界读（新增 `fbs_server::readable_bytes_from` 钳制，第一轮 B5 落地）；⑥iOS `rescanApps` 图标扫描线程池与 `bootDeviceAtIndex:` 系统重建竞争（新增 `session_mutex`，主线程入口只 try_lock 防 dispatch_sync 死锁）。另补 sensor 空驱动守卫（iOS 无 sensor driver，guest 查询即空指针崩溃）。验证：Release `ios_regression_test.sh` **8/8** + `angrybirds` 套件 **5/5**，日志无 panic。详见 [docs/ios-testflight-crash-triage-r2.md](./ios-testflight-crash-triage-r2.md)。 |
| 2026-07-14 | **修复安装/切换 Nokia 6680（EKA1）及启动应用时的两处 iOS 宿主闪退**：① iOS `rescanApps` 在 `applist_server` 惰性初始化前直接调用 `rescan_registries`；EKA2 注册表扫描未立即使用 FBS，长期掩盖了顺序错误，而 EKA1 AIF 图标扫描会在线程池中调用 `read_icon_data_aif → fbs_server::create_bitmap`，空 `fbsserv` 最终在 `pthread_mutex_lock(0x1e8)` 崩溃。桥接层现先通过 `get_registerations()` 完成服务依赖绑定，再刷新注册表。② EKA1 nearest-font design-height 请求按 Symbian 契约不携带 slot 2 的 max-height/device-size 描述符，FBS 却在 twips 换算时无条件解引用空 `optional<vec3>`，启动 Calculator 即 abort；现校验必需的 slot 0，并只在 slot 2 存在时换算。验证：Release 回归 **11/11**；RM-36 应用列表稳定显示 39 个应用，启动 Calculator 后宿主进程持续存活且无新增 crash report。Calculator 后续 guest 兼容问题另行跟进。 |
| 2026-07-10 | **修复应用列表图标缓存跨机型串用**：MIF 图标 debinarized SVG 磁盘缓存（`data/cache/icons/debinarized_<caption>.svg`）只按应用名命名、各机型共用，且新鲜度判定为「缓存 mtime ≥ mif mtime」——切 ROM 后旧机型生成的缓存永远更新，同名系统应用（X7 与 5320 图标资源不同）持续展示上一台设备的图标。修法：`iconPNGDataForUID` 按当前设备 firmware code 分子目录（`data/cache/icons/<code>/…`）。Swift 侧列表本就 `.id(currentIndex)` 整体重建、内存态无此问题；MBM/bitwise 图标每次现解不落盘，不受影响。验证（iPhone 16 Pro 模拟器）：rm-409 与 rm-707 各自生成独立缓存目录，17 个同名系统应用（Calculator/Clock/Messaging 等）两侧 SVG 哈希不同、互不覆盖；`ios_regression_test.sh`（Release）**8/8**。 |
| 2026-07-10 | **回归脚本新增 `angrybirds` 触屏套件**（`scripts/ios_regression_test.sh angrybirds`，不含在 `all` 内）：X7（rm-707）启动 Angry Birds（fullscreen 布局，经 `-LaunchKeypadLayout` 指定；不能传 `-EKA2L1RegressionMode`，它会强制 classic 布局），等待 splash 首帧 → **在加载画面点一次**（修复前此操作会永久废掉后续手势，正是回归防线）→ 等主菜单（整屏重绘判定，阈值 `AB_DIFF_MIN`）→ 点屏幕中心 PLAY 断言进入关卡选择 → 滑动轮播断言翻页 → 无 crash。触屏 guest 无 accessibility 元素，坐标点击/滑动经 xcodebuildmcp 内置的 `axe` HID 工具（自动定位）。验证：Release 构建 5/5 PASS，截图确认关卡页与翻页语义正确。 |
| 2026-07-10 | **修复 Symbian^3 触屏游戏（X7 Angry Birds）手势永久失效**：iOS 触摸的 `pointerId`（UITouch 地址）被截断为 guest 的 `TAdvancedPointerEvent::PointerNumber()`（uint8），Symbian^3 游戏按指针号索引触点槽位、垃圾指针号被丢弃或搞乱状态机——加载画面点一次 / 双指捏合引入新 UITouch 实例后手势全废；S60v5 无指针号故正常。修法对齐 Qt 前端：桥内维护 UITouch→0..7 槽位映射（press 分配 / release 归还 / 未知 release 丢弃）。详见阶段 3 修复清单第 14 条。验证：X7+AB 模拟器修复前 PLAY 全无响应、修复后菜单/滑动/弹弓/加载期间连点均正常；5320 回归通过。真机捏合待复核。 |
| 2026-07-09 | **iOS 应用列表体验优化：系统应用判定收紧 + 四列网格 + UID 复制菜单**。①"Hide System Apps"（默认开启）此前沿用 Qt 的 `land_drive==Z && uid<0x10300000` 启发式，导致 UID 落在用户区间的 ROM 内置应用（如内置游戏）漏过过滤仍然显示；改为仅以落盘盘符判定——凡驻留 ROM 盘（Z）即视为系统内置应用，用户安装包一律落在可写盘（C/E），故隐藏后只剩用户手动安装的应用。长按卸载选项本就仅对非系统应用出现，随判定收紧自动只对用户应用可用。②网格布局列宽从 `adaptive(minimum:100, spacing:16)` 收窄为 `adaptive(minimum:76, spacing:12)`，当前模拟器一行稳定容纳 4 列图标。③长按上下文菜单新增"复制 UID"项（`appContextMenu`，取代原 `uninstallMenu`）：菜单项标签即十六进制 UID（`0x%08X`，附 `doc.on.doc` 图标），点击写入 `UIPasteboard` 并弹横幅；对系统/用户应用均可见，用户应用额外保留 Uninstall。验证（iPhone 16 Pro 模拟器，rm-409）：默认视图仅 4 个用户应用且排成 4 列；长按 Brothers in Arms 菜单含"0x20004380 + Uninstall"，点复制后 `pbpaste` 得 `0x20004380`；开启 Show System Apps 后长按 Calculator（`0x10005902`）菜单仅有复制 UID、无 Uninstall；`ios_regression_test.sh` **8/8**。 |
| 2026-07-08 | **修复 TestFlight 崩溃 B2/B3：内核 teardown 期两处 completion UAF**。新增 `kernel_system::is_thread_alive(thread*)`（按指针查 `threads_`，线程析构会 erase）与 `is_wiping()` 两个原语。**B2**（`CG2S`，`notify_info::complete → property::cancel → ~property_reference`）：property 引用因句柄关闭析构时对订阅调 `complete`，订阅者线程可能早退出 → 悬垂 `requester` UAF；`property::cancel`/`notify_request` 在 `complete()` 前用 `is_thread_alive` 校验存活，不在则只移除订阅不 complete。**B3**（`Ckao`，`~system_impl → wipeout → session::destroy → detatch → semaphore::signal → dewait`）：wipeout 拆除期 `session::detatch` 仍 `signal_request()` 唤醒线程，`dewait` 触到已失效调度状态 → SIGSEGV；给消息完成块加 `!kern->is_wiping()` 守卫（仅正常关闭才唤醒客户端，wipeout 期跳过，`unref` 照常）。B1 音频 helper 也改用同一 `is_thread_alive`。验证：`build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**。详见 [`ios-testflight-crash-triage.md`](./ios-testflight-crash-triage.md) B2/B3。 |
| 2026-07-08 | **修复 TestFlight 崩溃 B1：音频 DSP 回调跨线程 UAF**（`CMJ`/`DLH`，`notify_info::complete` 崩在 CoreAudio 渲染线程）。`audio.cpp` 的 DSP 流 `more_buffer` 与播放器 `notify_any_done` 回调在音频渲染线程上 `info.complete()`，会解引用 `info.requester`（guest `kernel::thread*`）；当请求线程/进程在通知触发前被销毁即悬垂指针 UAF（原实现更先靠 `requester->get_kernel_object_owner()` 取 kern，崩在拿锁前）。改为把稳定 `kernel_system*`（`sys->get_kernel_system()`）捕获进回调，新增 `complete_audio_notify_if_alive`：`kern->lock()` 下先在 `kern->get_thread_list()`（线程析构会 `erase`）里校验 requester 存活才 complete，否则 `info.sts=0` 丢弃陈旧通知，锁序与原实现一致。验证：`build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**（Final Battle 含音频路径无回归）。详见 [`ios-testflight-crash-triage.md`](./ios-testflight-crash-triage.md) B1。 |
| 2026-07-08 | **修复 TestFlight 崩溃主因：applist 后台线程池并发 `create_bitmap` 破坏 fbs chunk 分配器**（导出集里出现最多的一族，~5 个崩溃点，`SIGSEGV @0x160`）。`applist_server::rescan_registries` 用线程池并行加载注册表（含 `read_icon_data_aif → fbs_server::create_bitmap`，上游 `c3ea261e9` 并行化），但 `create_bitmap`/`free_bitmap` 及通用/大块分配直接改写共享的 `shared_chunk_allocator`/`large_chunk_allocator`（`block_allocator`，无内部锁）→ 多 worker 同时分配破坏 free-list → `bitwise_bitmap` 近 null 指针 `+0x160` 解引用崩溃。给 `fbs_server` 加 `std::recursive_mutex allocator_lock_`，覆盖 `create_bitmap`/`free_bitmap`（含惰性 `initialize_server`，防双初始化竞争）/`allocate_general_data(_impl)`/`free_general_data_impl`/`allocate_large_data`/`free_large_data`/`load_data_to_rom`。这些均为 guest FBS IPC 事件、非每帧合成热点（合成走 driver 侧 `bitmap_cache`/`font_atlas`），默认关闭压缩队列时稳态无竞争，成本可忽略；顺带覆盖压缩线程既有潜在竞争。逐点归因（含音频 DSP 回调跨线程 UAF、GL 顶点/位图哈希越界读、退出 teardown、以及一批 iOS 27 beta 系统/SwiftUI 侧非本项目崩溃）见 [`ios-testflight-crash-triage.md`](./ios-testflight-crash-triage.md)。验证：`build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**。 |
| 2026-07-08 | **修复 N-Gage 安装器 "Installation complete" 弹窗在 iOS 不显示；深度定位 "Start Game" 黑屏（未修）**。详见 [`ios-ngage-installer-popup-and-game-launch.md`](./ios-ngage-installer-popup-and-game-launch.md)。核心结论：①**弹窗**——AVKON info note 走的 `redraw` 窗口在 iOS 上 `EndRedraw` 完成后从未按正确 z-order 合成（只走增量 client 路径，会被后面窗口覆盖或在整屏 server pass 后被跳过）；`redraw_msg_canvas::end_redraw` 在 `content_changed` 时置 `FLAG_SERVER_REDRAW_PENDING` 强制整屏 server 重合成即修复。附带修一处 Debug assert 崩溃：`mutex::wait` 的 `assert(!holding->wait_obj)` 在"持锁线程本身阻塞在别的等待对象"这一合法 Symbian 状态下 abort（Release/NDEBUG 早已 strip，assert 下方代码本就正确排队），命中于独立 "N-Gage Installer" app 路径。②**黑屏**（未修）——与 CPU 后端无关（dyncom/dynarmic 表现一致）：freeze 时调度器 idle dump 显示启动器线程 `ngiplay` 被 **suspend**（挂起时正处于 `User::After` sleep）且带 8 个未消费的已完成请求、永不 resume，是 PlayServer 游戏启动编排的 guest 侧挂起/恢复死锁；该 ONE 为 DRM 破解版（跑 `AS_2000AFBF` 激活 + 反复加载 OMA/WMDRM agent），而干净 N-Gage 游戏 Snakes（`0x2000730F`）在 iOS 正常进游戏，故通用路径无恙。dynarmic 另有 JIT 编译崩溃（`ConstantPropagation`→`FoldShifts`→`Value::IsImmediate` 沿长 `Identity` 链爆宿主栈），关 ConstProp 可绕过崩溃让游戏启动但仍撞同一挂起死锁，非真修故未保留。验证：弹窗在 dyncom 与 dynarmic 两后端均显示；`ios_regression_test.sh` **8/8**。 |
| 2026-07-08 | **SIS 安装改为原地进行，去掉 `Documents/sis/` 暂存拷贝**。安装器会把包内容全部释放到设备盘符，SIS 原文件装完即无用，拷贝纯属浪费空间。`handleSisImport`（`+` picker 与文件 App 打开共用）现直接持安全作用域对原始 URL 调 `installSis`；无设备时不再暂存、仅提示；`ImportRouter` 的 sis 分支成为死代码已删除（字体/zip/rom 分支不变）。验证（iPhone 16 Pro 模拟器）：删除旧暂存后从文件 App 冷启动打开 N-Gage 2.0 SIS → 安装成功且 `Documents/sis/` 未再出现拷贝，日志 "Installation done!"，无崩溃；`ios_regression_test.sh` **8/8**。 |
| 2026-07-08 | **文件 App 打开 SIS 直接自动安装（onOpenURL 接线）+ SIS 注册期非法盘符崩溃修复**。此前 Info.plist 已声明 `.sis/.sisx` 文档类型，但 SwiftUI 层没有任何 `.onOpenURL` 处理，从文件 App 点开 SIS 只会把 EKA2L1 带到前台。现 `ContentView` 挂 `.onOpenURL`：URL 先进 `pendingOpenURLs` 队列（冷启动时 onOpenURL 可能早于模拟器 boot），boot 完成后统一 drain——SIS/SISX 复用既有 `handleSisImport`（ImportRouter 落 `Documents/sis/` + `installSis` 装上当前设备 + 刷新应用列表 + 横幅），无设备时仅暂存文件并提示先装 ROM（空态页面补了横幅显示）；其他已注册类型（字体/ROM zip）走 `ImportRouter` 暂存。验证中发现共享代码真实缺陷：N-Gage 2.0 SIS 含 target 首字符非盘符（空 target）的文件记录，`fill_controller_registeration` 直接喂给 `char16_to_drive` → Debug 构建 assert abort（Release 下算出错误 drives 位掩码），已在调用处守卫（仅接受真实盘符，否则跳过并 WARN）。验证（iPhone 16 Pro 模拟器，rm-409）：文件 App 点开 N-Gage 2.0 SIS，冷启动与 app 已运行两条路径均自动安装成功（日志 2×"Installation done!"，applist 出现 N-Gage Installer/Games，无崩溃）；`ios_regression_test.sh` **8/8**。 |
| 2026-07-07 | **外设管理重构：多手柄支持 + 设置页外设列表 + 外设维度 Key Mapping**。新增常驻单例 `PeripheralManager`：跟踪全部已连接扩展手柄（实例级）+ 硬件键盘（单一条目，不区分具体键盘），维护「生效外设」——默认最新连接的设备自动生效，设置页「Connected devices」列表可点击切换（名称 + 圆圈/对勾），每行 ⓘ 进入该外设的 Key Mapping 页。只有生效外设驱动模拟器输入：`ControllerInputBridge` 给所有手柄挂 handler 但仅处理生效手柄的事件（外设集变化时 releaseAll 防卡键并重载映射）；键盘 `pressesBegan` 仅在生效外设为键盘（或无外设）时驱动 guest，`pressesEnded` 不门控避免切换瞬间卡键。映射存储改为外设维度嵌套 dict（`ios.peripheralKeyMappings` → `[deviceKey: [host token: scan]]`）：键盘统一 `keyboard`，手柄按 `gc:<vendorName>`（同型号共享映射，跨重连保留；GCController 无持久标识）；设备内一对一绑定，捕获页按外设类型只捕获对应输入（手柄页 gamepad 事件、键盘页 GCKeyboard 事件），文案/图标随类型切换，设备中途断开显示提示。移除 "Game controller" 开关（`ios.enableControllerInput` 废弃，默认监听）与 `startWirelessControllerDiscovery` 蓝牙扫描（配对手柄经常规 GameController 通知出现）；不做旧映射数据兼容（`ios.controllerMapping` 弃置）。验证（iPhone 16 Pro 模拟器，虚拟 Gamepad + GCKeyboard 均在列）：列表勾选切换、两外设各自映射页（手柄页 "Direction Pad (Up)/A Button"、键盘页 "↑/Return/F1"）、capture sheet 分型文案、Clear/Reset 及嵌套存储读写均正确；`ios_regression_test.sh` **8/8**。遗留：实体捕获按下、多手柄同连、生效切换的实际输入路由需真机手动验证。 |
| 2026-07-07 | **设置页新增游戏手柄 + 外接键盘按键映射编辑（guest 侧列表 + 按键捕获式绑定）**。新增 `ControllerMapping.swift`。交互按 guest 侧组织：映射页分「方向 / 功能键 / 数字键」三组列出全部 Nokia 按键（方向×4、OK、双软键、通话、挂机、Clear、0-9、`*`、`#`，各带 SF Symbol，通话/挂机绿红着色），每行显示当前绑定（"L1 Button · F1" 这类手柄+键盘并列）；点击行弹 capture sheet（"Press a button on the controller, or a key on the keyboard…"），按下实体手柄键或键盘键即绑定并保存，sheet 内可 Clear Binding / Cancel，页面底部 Reset to Defaults + 已连接手柄/键盘名。数据模型为 `[host token: guest scan]` 存 UserDefaults（`ios.controllerMapping`）：手柄键用 `HostButton` token（A/B/X/Y、L1/R1、L2/R2、Menu/Options、L3/R3、d-pad×4；左摇杆恒等同 d-pad 不单独绑定），键盘键用 `kb.<HID usage>` token——HID usage 是捕获（GameController `GCKeyCode`）与运行时（UIKit `UIKeyboardHIDUsage`）的共同货币，天然支持 iPad 外接键盘。冲突规则：同一 host 键后绑定的为准（dict 覆盖天然成立）；绑定时同时清掉该 guest 键同类（手柄/键盘各一）旧绑定，保持每 guest 键至多手柄+键盘各一个绑定；显示名优先取连接设备的 `localizedName`（Xbox "A Button"/DualSense "Cross Button"），键盘用内置 HID usage 名表。运行时：`ControllerInputBridge` 每次进模拟器画面时加载映射按 `HostButton` 全量轮询（左摇杆独立跟踪按下状态但共享 d-pad 绑定）；键盘 `scanCode(for:)` 先查映射（按 HID usage），未覆盖的键回落到不可绑定的文本输入（字母 multitap ASCII、空格/Tab、keypad `*`/`#`、Esc/keypad-Enter 便利键）；旧硬编码的方向/回车/F1-F4/数字/退格默认行为改由映射默认值承载（可编辑）。验证（iPhone 16 Pro 模拟器，连接虚拟 Gamepad）：`build_ios.sh simulator` 通过；映射页/三分组/图标/绑定名渲染正常；capture sheet 打开、Clear Binding 后行变 Not bound 且存储删除、跨重启持久；Reset 恢复默认；`ios_regression_test.sh` **8/8**。遗留：实体手柄/键盘的捕获路径需真机手动验证（模拟器无法注入 GCController/GCKeyboard 按键）。 |
| 2026-07-07 | **移除 "Rotate Screen" 功能 + iOS Swift/ObjC 层技术债清理**。界面方向已由键盘布局强绑定锁定（ngage 横屏、其余竖屏），仅旋转 guest 画面的 Rotate Screen 菜单项用途有限且拖着一套触摸逆映射复杂度，整体移除：系统菜单按钮与 `KeypadMenuActions.rotateDisplay`、bridge 的 `rotateGuestDisplayClockwise`/`resetGuestDisplayRotation`、`IosEmulator.mm` 的 `user_display_rotation` 与 `present_rect_*`/`present_guest_*` 原子组、`submitPointer` 的用户旋转逆映射分支及 `emulator.rotate` 本地化串；guest 自身 `ui_rotation` 的呈现旋转不受影响。技术债清理：`ControllerInputBridge` 删除按 element 分派的整套重复映射（`updateButton` 本就只在状态跳变时发键，统一走 `handleAll` 重同步）；长按/捏合手势的魔法扫描码（0xA7/0x10/0x11）改用 `Scan` 常量；`ContentView`/`SettingsView`/`ImportRouter` 三处重复的 Documents 根路径函数收敛为 `EKA2L1Bridge.swift` 中共享的 `documentsRoot()`；`AppGridCell`/`AppRow` 重复的异步图标解码逻辑抽成共享 `AppIconView`；`VirtualKeypad` 去掉单子视图 `ZStack` 包装。净 -177 行。验证：`build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**（FBattle 进游戏无 crash，Calculator 渲染/输入/Options 开关正常）。 |
| 2026-07-07 | **修复 dynarmic 下 Snakes 启动即 `E32USER-CBase 46`（stray-signal）panic；dynarmic 稳定性验证 + dyncom/dynarmic 性能 A/B**。dynarmic 开启后 Snakes 确定性在启动画面 panic（dyncom 正常）。经探针定位（`notify_info::complete` 完成源标记 → 双重完成排除 → `wait_for_any_request` 吸收路径插桩 + SVC 入口 PC 校验）确认根因是**既有 stray 信号在两个后端下都存在（dyncom 一次启动 ~235 个被吸收机制吃掉），但吸收机制在 dynarmic 下完全失效**，两个叠加缺陷：① stub 识别读的 `thread::ctx` 快照只在阻塞换出时刷新——dyncom（慢）下 stray 总在线程 park 后到达、快照准确；dynarmic（快）下 stray 在 guest 到达 WaitForAnyRequest 前已入队、等待走非阻塞路径、快照还停在上一个 SVC 位置 → 识别必失败。修复：进入 `wait_for_any_request` 时若为当前运行线程，先从活核心 `save_context` 刷新 `ctx`（SVC 入口活 PC 经探针验证两后端均准确）。② post-wait 的 stub 识别硬编码不接受 fast-exec stub（`allow_fast_wait_for_any=false`），而本 ROM 的 AVKON 调度器恰经 fast stub（`0x803A034C`）等待——panic 前最后一击就是 r0=0 的 fast-stub 直接等待被拒绝吸收。修复：post-wait 也允许 fast stub，wrapper 保护不变（r0 有效映射 → `User::WaitForRequest` wrapper → 不吸收，guest 循环自吸收；历史"absorb 扩到 fast-SVC 吞按键死锁"死于无此判别）。**对 dyncom 的影响**：共享路径，方向更正确（快照刷新消除同源陈旧问题；fast 吸收带同样保护），实测无回归。验证矩阵（final build，iPhone 16 Pro sim）：dyncom 回归 **8/8** + Snakes gameplay 无 panic + BIA **7/7**；dynarmic 回归 **8/8** + Snakes 进 3D gameplay 无 panic + BIA **7/7**；无宿主 crash report。**性能 A/B（Release）**：Snakes gameplay dyncom 42 vs dynarmic 40 FPS（渲染/present 受限，后端中性）；BIA gameplay dyncom 36-40 vs dynarmic 40 FPS（dynarmic 稳定满帧，dyncom 靠此前全部解释器优化才接近）；**Debug 构建差距巨大**（Snakes：dyncom 16 vs dynarmic 42 FPS——解释器受宿主 -O0 拖累，JIT 产物不受影响），对开发迭代显著友好。Calculator 历史 dynarmic SIGSEGV 本轮多次运行未复现（回归含完整 Options 菜单流程），保持观察。**注意吸收机制是补偿层而非根治**——stray 源头（同步 IPC 完成记账失衡为主）未修，根因模型、探针配方与记账重构路线见 [`stray-signal-accounting-followup.md`](./stray-signal-accounting-followup.md)。 |
| 2026-07-07 | **iOS JIT（dynarmic）支持：仅 sideload/unsigned IPA 编入，设置页开关切换**。构建面：原 `EKA2L1_IOS_SIMULATOR_DYNARMIC` 与新增的设备 JIT 开关精简合并为**单一** CMake 选项兼编译宏 `EKA2L1_IOS_DYNARMIC`（模拟器/真机统一；模拟器默认 ON、设备默认 OFF）；`build_ios.sh` 按构建风味给默认值——simulator 与 unsigned `device`（sideload IPA，`ios-unsigned-ipa.yml` 产物）为 ON，signed 构建 OFF，`archive`（TestFlight/App Store）硬编码 OFF，可用 env `EKA2L1_IOS_DYNARMIC` 覆盖——分发构建不编入任何 JIT 代码。运行时面：`arm::host_can_jit()` 在 iOS 上做真实探测（mmap RW → 写入 `ret` → mprotect RX → 执行；越狱外设备仅 CS_DEBUGGED 进程即 debugger/JIT enabler 使能后才成功），dynarmic 的 arm64 后端经 oaknut `CodeBlock` 已内置 `TARGET_OS_IPHONE` 的 RX 映射 + RW/RX mprotect 切换路径故无需改内存管理；`resolve_emulator_type` 在无权限时带原因回落 dyncom。**opt-in 语义**：新增独立配置项 `ios-use-jit`（默认 false）而非复用 `cpu_backend`——后者桌面默认 "dynarmic" 且可能已持久化在存量 config.yml，直接尊重会把 iOS 默认后端偷偷切成 dynarmic（实测触发回归 7/8，Calculator 默认渲染空白）；`epoc.cpp` 仅当 `ios_use_jit && host_can_jit()` 才选 dynarmic，并在启动时打一行后端选择日志。前端：设置页新增 System 分组 “JIT recompiler (dynarmic)” 开关（仅 `jitCompiledIn` 构建显示；footer 按 `jitAvailable` 提示需 debugger/JIT enabler），改动经 `needs_reboot_before_launch` 在下次启动 app 时生效。验证（iPhone 16 Pro 模拟器）：unsigned 设备构建含 11013 个 Dynarmic 符号且无签名、JIT=OFF 配置确认 “linking without dynarmic”；默认态回归 **8/8** 且日志 `dyncom (opt-in false)`；UI 开开关后 `data/config.yml` 写入 `ios-use-jit: true`、ZipManager 在 `dynarmic (opt-in true, permission true)` 下正常渲染；关回后回落 dyncom。遗留：真机 JIT-enabled 环境实测、dynarmic A32 崩溃（Calculator）未修故保持 opt-in 实验性。 |
| 2026-07-07 | **iOS 虚拟键盘与游戏界面大改**：游戏界面全屏化（隐藏导航栏/状态栏，键盘悬浮覆盖画面而非压缩画面）；键盘重构拆出 `KeypadComponents.swift`（滑动式方向键、共享按键原语、数字键盘）与 `VirtualKeypad.swift`（各布局）。新增两种布局：`ngage`（横屏 N-Gage QD 风格，左五向键+左软键+系统键、右数字键+右软键+清除键、画面居中，进入自动转横屏）与 `fullscreen`（S60v5 触屏 ROM，画面 content-fit 满屏、仅右下角系统键）。所有布局去掉通话/挂机键、补清除键（全屏布局除外）、软键统一用 `l/r.button.roundedbottom.horizontal`（iOS17+）或 `l/r.circle`，新增系统功能键弹 SwiftUI 原生菜单（切换键盘/调透明度/横竖切换/存截图/重启/退出）。方向键改为按住滑动可切换方向。启动参数 `-LaunchKeypadLayout` 指定布局便于测试；触屏 ROM（`epocver>=epoc94`）默认走 `fullscreen` 布局（独立偏好键 `ios.keypadLayout.touch`）。**界面方向与键盘布局强绑定并锁定**：ngage 横屏、其余竖屏，由 `AppOrientationDelegate.supportedInterfaceOrientationsFor` 读 `lockedInterfaceOrientationMask` 强制，设备物理旋转不改；"Rotate Screen" 菜单改为只旋转 guest 画面本身（core `user_display_rotation` 叠加到 `submit_screen_frame` 的呈现旋转），不动界面；触摸映射同步旋转（`submit_screen_frame` 发布呈现矩形 + guest 原始尺寸，`submitPointer` 在旋转非 0 时把触点归一化后逆旋转成 guest 逻辑坐标并 `raw_screen_pos_`），切键盘布局时重置旋转。移除失效的 Settings 方向选择器。classic 布局定型为左右两组对齐两边、DPad 大圆居中、LSK/RSK/系统/清除四键嵌于圆的四角（纵向留间隙不相切），cluster 上下边缘与右侧数字键盘对齐。**修复切换布局尺寸变化后画面扭曲**：`attachLayer` 在同一 layer bounds 变化时补 `update_surface_size` + `re_present_screen`（新增 `present_mutex` 串行化 present 防止 launch/resize 与 os 线程并发抢 slot fence 死锁）；画面顶部锚点 `setDisplayAnchorTop` 让底部键盘覆盖 letterbox 而非游戏。**根因修复键盘点击失灵**：全屏后 render view 铺满整屏、在触摸命中测试上盖过 SwiftUI 键盘叠加层，键盘按键收不到点击——让 render view 的 `hitTest` 仅在键盘覆盖区域（SwiftUI 经 GeometryReader 上报的 `.global` frame）让出触摸、其余区域保留 guest 触摸即可（宿主仍是 `EmulatorViewController` / `UIViewControllerRepresentable`，与命中测试无关）。验证：`scripts/ios_regression_test.sh` 8/8 PASS；四种布局 + 横竖切换 + 系统菜单各项均截图确认。 |
| 2026-07-06 | 修复 iOS 主界面右上角 `+` 点击后不弹文件选择器：主界面同时挂 SIS 与 N-Gage 两个 `.fileImporter`，触发 SwiftUI presentation 冲突；改为 `HomeImportTarget` + 单一 home importer 多路复用。验证：`./scripts/build_ios.sh simulator` 通过；booted iPhone 16 Pro / iOS 26.5 安装 Debug build，XcodeBuildMCP 点击 `Add` 后截图确认 UIDocumentPicker 弹出。 |
| 2026-07-05 | **补齐首批 iOS 产品化 P0/P1**：参考 `muhannad-ios` 的 Icon Composer 风格素材生成 iOS App 图标和启动屏；版本号改为年月制 `26.7.0` / `260700`；新增 `PrivacyInfo.xcprivacy`（不跟踪、不采集数据，并声明 UserDefaults、文件时间戳、磁盘空间、系统启动时间 required-reason API）；顶层 iOS deployment target 固定 16.0。SwiftUI 侧新增首次使用引导，接入英文 String Catalog（CMake 标记 `text.json.xcstrings` 并开启 Xcode String Catalog/Swift 字符串提取设置），设置页改即时保存并隐藏 CPU backend/JIT/log filter 等开发项，补日志导出、清除数据、存储占用；游戏会话补 idle timer、音频中断恢复、退出/重启菜单、截图保存到相册，并从 app row 去掉 `uid=0x...` 调试文案。`docs/ios-productization-checklist.md` 已按完成/延期状态更新；合规红线与 App Store 页面素材按发布前复核延期。 |
| 2026-07-05 | **新增产品化 checklist**：`docs/ios-productization-checklist.md`，按 P0（上架硬门槛：App 图标/启动屏/隐私清单缺失、无 JIT 合规、部署目标 18.0 需下调、dynarmic 选项埋雷）→ P1（onboarding + ZIP 固件导入、本地化、设置即时生效、idle timer/文本输入）→ P2（音频收尾、字体引导、iPad、Metal/ANGLE）→ P3（CI/TestFlight）列出离 App Store 上架的全部差距；阶段总览下加引用。基于 iPhone 16 Pro 模拟器实测（首页/游戏内/设置三处截图）+ 代码审视得出。 |
| 2026-07-04 | **修复 iOS Brother in Arms 3D (`0x20004380`) 标题/欢迎页整体发蓝**。根因：iOS GLES 路径此前为规避 simulator 对部分低位深纹理的问题，把 32bpp bitmap texture 也创建为 RGBA8，但 `update_bitmap()` 只对 8/16/24bpp 做 CPU 展开；Symbian 32bpp framebuffer/bitmap 内存实际是 B,G,R,A，iOS 直接按 RGBA 上传后红蓝通道互换。桌面/Android 仍可用 `GL_BGRA`，所以只在 iOS bitmap 上传分支把 32bpp 纳入现有 BGRA→RGBA 展开逻辑，保持其它平台不变；同时对展开前的尺寸乘法和源数据长度做校验，避免异常 update 命令在 CPU 侧分配超大临时 buffer。验证：安装 Debug simulator app 后启动 `-LaunchROMCode rm-320 -LaunchAppUID 0x20004380`，在 `Do you WANT SOUND?` 点 LSK/YES，标题页恢复暖色/军绿色，不再蓝青；`./scripts/build_ios.sh simulator` 通过；`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle + Calculator），且本轮未新增 `EKA2L1` crash report。 |
| 2026-07-04 | **修复 iOS Brother in Arms 3D (`0x20004380`) 在 `Do you want sound?` 选择 YES 后宿主 native crash**。根因：YES 后游戏走 MIDI 播放路径，`eaudio_player_supply_url()` 创建 `player_tsf`，但 iOS bundle/sandbox 没有 `resources/defaultbank.sf2`；`player_tsf` 的 `load_bank_from_file()` 返回 null 后仍无条件调用 `tsf_channel_set_bank_preset(synth_, ...)`，TinySoundFont 在空 `tsf*` 上读 `0x18` → `EXC_BAD_ACCESS / SIGSEGV`。修法：iOS CMake 将 `src/emu/drivers/resources/defaultbank.sf2` 打进 `.app/soundfonts/`，`IosEmulator` 启动时随 shaders 一起 stage 到 `Documents/data/resources/defaultbank.sf2`；共享 TSF player 对 SF2 加载失败做防御，`open_url/open_custom/play/data_callback` 不再对空 synth 调 TSF；`eaudio_player_supply_url/supply_buffer` 只在本次打开成功时替换/使用 player，否则返回 `error_not_supported`，避免失败 player 被塞进 `impl_` 后继续解引用。顺带修正 TSF `set_repeat()` 的 `silence_intervals_us_` 自赋值。验证：修前 crash report 栈为 `tsf_channel_init` ← `tsf_channel_set_bank_preset` ← `player_tsf` ← `new_audio_player` ← `eaudio_player_supply_url(audio.cpp:187)`；修后安装 Debug simulator app，先删除 sandbox 旧 SF2，启动 BIA 自动 stage 回 `defaultbank.sf2`，点 LSK/YES 进入 title screen，无新 `EKA2L1` crash report；`./scripts/build_ios.sh simulator` 通过；`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle + Calculator）。 |
| 2026-07-04 | **修复 iOS 主页切换设备后 app 图标错乱**。根因是 SwiftUI app grid/list 的 `ForEach` 用列表 offset 当 identity，5320→N95 这类设备切换后相同位置的 cell 会保留旧 `@State icon`；同时后台 icon 解码回调没有校验当前 UID，可能把旧 UID 的图片写回已复用的新 cell，表现为 Zip manager 显示 Snakes 图标、重启后恢复正常。修法：grid/list 改用 app UID 作为 item identity，并在 `currentIndex` 变化时重建列表 subtree；`AppGridCell`/`AppRow` 改用 `loadedUID` 跟踪当前图标，UID 变化时清空并重载，异步回调返回时校验 UID 后再写入。验证：`./scripts/build_ios.sh simulator` 通过；`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle + Calculator）。 |
| 2026-07-04 | **修复 iOS app list 中 Snakes (`0x2000730F`) 重复显示**。根因在共享 `applist_server`：新架构扫描 `Private\10003a3f\apps` 与 `Private\10003a3f\import\apps` 时会把同一语言族的多个 `.r??` 注册资源并行交给 `load_registry()`，而旧代码只在读取前按 `rsc_path` 检查一次，最终 `regs.push_back()` 不在同一临界区内重查，竞态下同一 UID 可插入两次；iOS 前端只是原样渲染 `get_registerations()`。修法是在 applist 后端新增最终提交 helper，在持有 `list_access_mut_` 时按规范化 `rsc_path` 和非零 app UID 去重；同 UID 冲突时优先保留非 Z 盘注册项，避免用户安装覆盖被 ROM 项抢占。`Found app` 日志只在实际提交后打印。验证：安装新 Debug simulator app 后清日志启动 N95，Snakes 日志只出现 1 次且无重复 Found-app UID；`./scripts/build_ios.sh simulator` 通过；`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle + Calculator）。 |
| 2026-07-03 | **补齐第二批 iOS 前端产品功能：ROM zip bundle 导入 + N-Gage game-card 文件夹导入**。`ImportRouter.swift` 的 `.zip` 分支不再报未支持，而是通过新增的 `EKA2L1Emulator.unzipArchive(atPath:toDirectory:)` 调用项目已有 `miniz` 解包到 `Documents/roms/<zip basename>`；解包先落 `Documents/import_tmp`，成功后原子替换目标目录，并拒绝绝对路径/`..` 路径防 zip traversal。主界面菜单新增 “Install N-Gage Game”，用 iOS 文件夹 picker 选择 game-card folder，`IosEmulator` 新增 `installNGageGameAtFolderPath:` 窄桥接，调用 core 的 `system::install_ngage_game_card()` 并返回错误码/游戏名，SwiftUI 后台执行、完成后刷新 app list。验证：`./scripts/build_ios.sh simulator` 通过；`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle 进游戏无 crash，Calculator 渲染/输入/Options 开关正常）。未做：单文件 `.n-gage` drop-to-`E:\n-gage` 与 iCloud Progress Sync 仍需后续拆分。 |
| 2026-07-05 | **兄弟连（BIA 3D）模拟器 gameplay 36→38 FPS：dyncom 解释器第二轮去间接化（Thumb 场景）**。BIA 与 Snakes 不同：模拟器里渲染线程 76% 空闲、解释器线程 ~100% 饱和，纯 CPU 受限；且 BIA 是 Thumb 密集代码，热点分布不同。按 `sample` 剖析逐项收割（全部沿用「偏置指针比较 + 内联主导形式」的已验证套路）：(1) `ADD_INST` 原本在 Thumb 模式下**每次执行**都 `ReadCode`+`GetThumbInstruction` 重读代码区分 ADR/ADD——改为仅 `Rn==15` 时才判别（普通 ADD 直接读寄存器），消掉 ~6% 的 ReadCode 热点；(2) `CondPassed` 编译期生成 16×16 位掩码查表 + 强制内联（原 switch 不被内联，占 ~4.5%）；(3) `SHIFTER_OPERAND` 快路径从 imm 扩展到 Register/LSL#imm/ASR#imm/LSR#imm 四个主导形式；(4) `LS_GET_ADDR` 增加寄存器偏移分支，新增半字/双字家族专用 `MLS_GET_ADDR`（imm/reg 偏移内联），LDM/STM 改直接调用；(5) `LdnStM*` 的逐位寄存器计数循环换 popcount。验证：`cpu_difftest` 5 种子共 1.9M 用例 PASS、`ios_regression_test.sh` 8/8、`ios_bia_gameplay_test.sh` 7/7、BIA 战斗场景 FPS 稳定 38（改前 32-36）。**已探明不值得做的**：L1 块缓存扩容 8192+乘法散列（实测中性偏负，已回滚）；DISPATCH 代价倍增实验显示分发探测总代价 ≤1 FPS，故块链接/指令融合上限不足 1 帧（guest 指令剖析：块均长 8.74 条、`CMP→B_COND_THUMB` 6.6% 第一对，与 Snakes 结论一致）。解释器体系内到达实际地板，满帧 40 的余下 2 帧需要 JIT（iOS 不可用）或 euser `Mem::Copy`/BITGDI 级 HLE（LDR 22.6%/STR 6.6%/STRB 3.2% 提示 guest 软件拷贝占比高，为后续方向）。 |
| 2026-07-05 | **BIA 满帧调查收尾:Mem::Copy/BITGDI HLE 方向经数据验证后关闭**。给 opt-in 的 dyncom 指令剖析器(`EKA2L1_DYNCOM_PROFILE`)新增 guest PC 直方图(64B 桶、每 64 条指令采样、首次命中即快照代码字节——dump 时跨进程读会触发 guest 数据中止,曾把 BIA 打成 KERN-EXEC 3,已改为采样时快照)。BIA gameplay 剖析结果:热点高度集中且 **100% 落在 `bia3d.exe`(run=0x70000000, size=0x56DEC)内部**——`0x7004BD40` 单桶 14.3% 是逐像素 RGB565→32 位转换循环,`0x7003Cxxx` 簇 ~40% 是引擎的纹理扫描线光栅化,ROM DLL(euser/bitgdi)与补丁 DLL(0x84xxxxxx)均无热点。结论:(1)导出级 HLE(euser `Mem::Copy`/BITGDI)对 BIA 覆盖率为零,方向关闭;(2)顺带做了「屏幕模式强制 EColor64K 消除 565→888 转换」实验,结果 guest 反而执行更多指令(38→19 FPS,负收益),已回滚。BIA 模拟器 38 FPS 即解释器体系内的实际上限;最终态回归 8/8、BIA 38 FPS 复核通过。 |
| 2026-07-05 | **BIA 达到满帧 40:dyncom 翻译期批量循环加速(通用机制,非游戏特定)**。上一条调查确认 BIA 的 14.3% 热点是游戏内嵌的逐像素 RGB565→32 位转换循环后,按「翻译时识别标准转换/拷贝循环并批量执行」实现:Thumb 块翻译时若识别出规范 do-while 拷贝/转换/填充循环(单 load、目的存储稠密平铺、读先写寄存器自仿射、无读标志指令、`SUBS/CMP+BNE` 计数器),用单迭代符号执行证明形状后,在块首插入合成指令(idx 205),运行时经 dyncom TLB 按页分块批量执行 **最多 N-1 次**迭代——**最后一次迭代永远留给解释器**,标志位/临时寄存器/循环退出全部来自真实解释,批量步只回写归纳寄存器(`r += delta*M`),TLB miss 即停、余下交回解释器(故障/未映射保持解释语义),配额按 `body_len*M` 记账。默认开启,`EKA2L1_NO_LOOP_ACCEL=1` 关闭。验证:同构建 env A/B——开 40/38/38/40(触顶 40),关 38/38/36(=旧基线);`cpu_difftest` 500k PASS;回归 8/8;BIA 脚本 7/7;开/关 3D 入场画面像素级目视一致(转换无损);Snakes 冒烟进 3D gameplay 42 FPS 无 panic。BIA 最终:32-36 → **39-40 FPS(满帧)**。 |
| 2026-07-03 | **补齐第一批 iOS 前端产品功能：GameController 手柄输入**。参考 MuhannadYT/EKA2L1_IOS 的硬件输入能力，但保持本仓库 SwiftUI + ObjC bridge 架构不变：`EmulatorViewController.swift` 新增 `ControllerInputBridge`，在 emulator screen 出现期间监听控制器连接/断开，把 d-pad/左摇杆、A/B/X/Y、肩键、扳机和 Menu 转成现有 `submitRawKey` 扫描码；离开 screen 时释放所有按下状态，避免 guest 卡键。`SettingsView` 增加 “Game controller” 开关（默认开），`src/emu/ios/CMakeLists.txt` 链接 `GameController.framework`。验证：`./scripts/build_ios.sh simulator` 通过；在带 `rm-320` ROM 与测试 app 的 iPhone 16 Pro simulator 上，`scripts/ios_regression_test.sh --install build/ios-simulator/src/emu/ios/Debug-iphonesimulator/EKA2L1.app` **8/8**（Final Battle 进游戏无 crash，Calculator 渲染/输入/Options 开关正常）。本批只补低耦合输入功能；备份/同步、传感器 passthrough 和 per-game settings 仍按后续批次拆。 |
| 2026-06-21 | **修复 5320（RM-409）计算器在启用 AknIconServer HLE 后整页不显示；把 HLE 限定到真正需要它的 N95 世代**。背景：commit `e1cc135` 为修 N95（RM-320，S60v3 **FP1**=`epoc93fp1`）Options 菜单不弹（guest 用软件 OpenVG 渲染可缩放 NVG 菜单图标，模拟器无 GPU NVG，其 draw-device 只收 32bpp 故对 64K 图标 `User::Leave` → 菜单构建中止）而**全局**启用了自带的 `akn_icon_server` HLE（`services/init.cpp`，完全替换 guest 图标服务器）。但该 HLE 并非完整替代：切到 5320（S60v3 **FP2**=`epoc93fp2`）后计算器主体黑屏。逐层定位：①guest 发 `preserve_icon_data`(opcode 0x3) 等 HLE 未实现的 opcode，而 `fetch` 的 `default` 分支只 `LOG_ERROR` 不 `ctx->complete()` → guest 同步 `SendReceive` 永久阻塞 → 黑屏；②即便补完成，运算符仍不显示：运算符容器 `avkon2.mif idx40` 是 mif 内嵌栅格（HLE 的 .mif 矢量路径明确不处理 bmp 型条目），且 `get_content_dim`(opcode 0x2) 的语义/描述符布局按 FP1 假设写就，给 FP2 回写 `TSize` 反而把皮肤背景画成白屏——经实测三态对比（无 HLE=红底+运算符正常；HLE 仅完成=红底无运算符；HLE 回写 content_dim=白底无运算符）确认 HLE 对 FP2 的图标协议是**不完整且不兼容**的，而 guest 服务器在 N95/5320 上都渲染完美。结论：**N95 菜单必须用 HLE（实测无 HLE 时 Options 弹窗不出现），5320 用 HLE 必坏**。修法（最小、低风险）：不再全局替换，仅当 `sys->get_symbian_version_use() == epocver::epoc93fp1` 时才 `CREATE_SERVER(akn_icon_server)`（对齐 `eikappui`/`skin`/`oom_app` 既有的 epocver 门控写法），其余 ROM（含 5320 及所有非 FP1）回落到自带 guest 图标服务器（即 commit 前的已知良好状态）。icon HLE 代码本身**一行未改**，保持 N95 已验证行为。验证：5320 计算器运算符(+−×÷√%±)与皮肤完整显示、HLE 不再介入（日志 0 条 AknIcon 未实现 opcode）；`scripts/ios_regression_test.sh` **8/8**（N95 Calculator Options 弹窗正常开/关、Final Battle 进游戏、均无 guest 崩溃）。遗留：把 HLE 做成对 FP2/v5 也完整的图标协议（mif 内嵌 bmp 解码 + 正确的 get_content_dim 语义）属独立工程，未做。 |
| 2026-06-21 | **修复兄弟连（Brothers in Arms 3D，`0x20004380`）一打开就闪退（宿主原生崩溃，非 guest panic）**。根因：`mbm_file::do_read_headers()`（`loader/src/mbm.cpp`）处理 `index_to_loads` 时不做边界检查——FBS 客户端 `load_bitmap_impl` 把 guest 请求的 `bitmap_id` 直接 `push_back` 进 `index_to_loads`，而 `BIA3D.mbm` 只有 2 张位图（有效索引 0/1），游戏却请求 `id=2`，lambda 内 `trailer.sbm_offsets[index]` / `sbm_headers[index]` 越界。该越界在桌面/Android 的非加固 libc++ 上是静默 UB（`operator[]` 读到垃圾），但 iOS 用的**加固 libc++** 会在越界 `operator[]` 直接 `__libcpp_verbose_abort` → SIGABRT，整个宿主 app 闪退。崩溃栈：`abort` ← `vector::operator[]` ← `do_read_headers()::$_0` ← `load_bitmap_fast` ← `fbscli::fetch`。修法：在 `index_to_loads` 循环里对每个索引加 `if (i >= trailer.sbm_offsets.size()) continue;`，越界请求安全跳过（不污染整个 MBM 解析）；后续调用方 `load_bitmap_impl` 的 `is_header_loaded(bitmap_id)` 检查会正常返回 `error_not_found`，guest 自行处理。跨平台修正一个真实越界（各平台共有的潜在 UB，仅 iOS 顶出为崩溃）。验证（booted iPhone 16 Pro）：修前 `id=2` 请求即 SIGABRT 崩溃报告；修后同一 `id=2` 请求安全跳过、继续成功加载 `id 0/1`、app 持续运行进入游戏初始化、无新崩溃报告、无 guest panic；`scripts/ios_regression_test.sh` **8/8**（Final Battle + Calculator 无回归）。 |
| 2026-06-15 | **HLE 方向梳理 + VFP 浮点走宿主原生（广义优化，非 Snakes）**。盘点已有 HLE：GLES1/2、EGL、OpenVG 已在宿主 GPU 原生跑（`dispatch/libraries/`）、窗口服务绘图原生（`services/window/classes/gctx.cpp`）、`fast_blit`/`update_screen`、音视频/相机及十余个设备服务 patch（`src/patch/`）；HLE 挂钩 = `patch_rom_export` trampoline（`libmanager.cpp`）把 guest 导出重定向到 patch 后的 guest 代码或 `BRIDGE_REGISTER_DISPATCHER` 原生函数。落地其中一项：dyncom 的 VFP 是 bit 精确的 ARM softfloat 参考实现（慢），在 `vfp_single_cpdo`/`vfp_double_cpdo` 加宿主原生快路径——标量、默认 FPSCR 模式（就近舍入、无 flush-to-zero/default-NaN）、且操作数与结果均为规格化时，宿主 arm64 的 `FADD/FSUB/FMUL/FDIV`（float/double）即 IEEE-754 就近舍入的唯一正确结果，与 softfloat 逐位一致，开销只是其零头；其余情形（NaN/Inf/非规格化/零、非默认模式、向量、MAC、cvt）回落 softfloat 保持精确语义（提交 `46ce517bd`，env `EKA2L1_NO_VFP_HOST` 门控，默认开）。验证：构造性正确 + 整数差分 80 万例不受影响 + `ios_regression_test.sh` 8/8 + Final Battle 帧在 ON/OFF 下逐像素一致（ImageMagick AE=0）。**无 FPS 数字**：现有标题（Snakes/Calculator/Final Battle）是定点/极少用 VFP，故在它们上不体现——这是给浮点密集标题的广义收益。其余 HLE 方向（per-title 热循环原生化＝Snakes 唯一真杠杆但属 app 专用、CFbsBitGc 位图基元、euser Mem/RHeap）见 [`ios_snakes_perf.md`](./ios_snakes_perf.md)。 |
| 2026-06-15 | **解释器去间接化：内联热路径的立即数寻址/移位操作数，Snakes ~37.5 → ~40+ FPS（首批越过噪声的微优化）**。此前 fusion / 移位特化 / block-cursor 都实测中性——因为开销在指令*执行*而非 dispatch，且分散。定位到真正的可削项：每条单 load/store 经 `inst_cream->get_addr`、每条数据处理经 `inst_cream->shtop_func` 走**函数指针**，在共享 handler 调用点上是**多态间接分支**（易误预测）且阻止内联。①**`LS_GET_ADDR`**（提交 `747719516`）：对主导的「立即数偏移、无回写」形式 `[Rn,#±imm12]`，用一次良预测的指针比较（`== LnSWoUBImmediateOffset`）内联那段平凡地址计算，其余寻址模式回落函数指针。A/B（Release，单构建 env 门控）：ON ~39.5 vs OFF ~37.3 = **~+2 FPS（~6%）**，分布清晰分离。②**`SHIFTER_OPERAND`**（提交 `52ab75ece`）：同款手法用于立即数移位操作数（`#imm` 的 rotate），指针比较 `== DataProcessingOperandsImmediate`，用语句表达式保持其作为表达式可用；与早先「中性」的移位特化不同点在于**在调用点内联、常见路径零调用**（旧版保留了未内联的 helper 调用）。A/B（叠加在 LS 之上）：ON ~40.3 vs OFF ~38.4 = 再 **~+2 FPS（~5%）**。两者叠加，**累计 ~37.5 → ~40+ FPS（~7%）**；各自 dyncom 差分 80 万例 + `ios_regression_test.sh` 8/8。去间接化对*主导*形式已基本吃满；更大的下一杠杆是 lazy-PC（消除每条指令 `Reg[15]+=size` 的写，blast radius 最大但更易出错）。详见 [`dyncom_optimization_plan.md`](./dyncom_optimization_plan.md)。 |
| 2026-06-15 | **Metal（ANGLE）Phase 2 完成：iOS 经 MetalANGLE 真渲染上屏，回归 8/8；但模拟器 FPS 中性**。新增 `gl_context_angle`（`drivers/...backend/context_angle.{h,mm}`，基于 MetalANGLE `MGLContext`+`MGLLayer`，MGL 自管默认 FBO/深度/呈现；`make_gl_context` 在 `EKA2L1_IOS_ANGLE` 下选它，提交 `84196e10c`）。前端接线：`attachLayer:` 在 ANGLE 下把一个 `MGLLayer` 作为 CAEAGLLayer 的 sublayer 托管、作为 `render_surface` 喂给驱动（全部 MetalANGLE 用法留在 ObjC++ 桥里，规避 Swift↔非模块化框架的 modulemap 摩擦；提交 `4ee9d67e0`）。**实测（Release，`EKA2L1_IOS_USE_ANGLE=ON`，booted iOS 26.5）**：Calculator 与 Snakes 3D 经 ANGLE→Metal 正确渲染，`scripts/ios_regression_test.sh` **8/8**（含 Calculator Options 菜单、Final Battle 进游戏、无 guest 崩溃）。**Snakes FPS A/B：ANGLE ≈ EAGL ≈ 38，中性**——并非预期的提升。原因：present 早已双缓冲，软件 GLES 的渲染延迟本就被藏在 guest 关键路径之外，~38 FPS 的真正瓶颈是解释器；把渲染换成硬件 Metal 因此不抬升模拟器帧率（但把三角形填充从 host CPU 核挪到 GPU，利于开发期散热/功耗）。ANGLE 的现实价值收敛为：Metal 上渲染正确性、对 Apple 弃用 GLES 的前瞻性、模拟器硬件加速。仍在默认 OFF 开关后；待真机（iPhone Air）A/B（真机 EAGL 本就硬件加速，预计同样中性）后再决定是否翻默认。详见 [`ios_metal_angle_plan.md`](./ios_metal_angle_plan.md)。 |
| 2026-06-15 | **Metal（ANGLE）Phase 0 spike 落地：ANGLE-over-Metal 在 iOS 模拟器跑通硬件 Metal**。选定并 vendored **MetalANGLE `gles3-0.0.8`** 预编译库（不入 git，由 `scripts/fetch_metalangle.sh` 按需下载+打包，类比 FFmpeg 的 out-of-tree 方式；`src/external/metalangle/` 已 gitignore）。新增构建开关 `EKA2L1_IOS_USE_ANGLE`（默认 OFF，经 `build_ios.sh` 透传），ON 时链接+嵌入 xcframework 并编译 Phase-0 smoke（`AngleSmoke.mm`：surfaceless GLES3 `MGLContext` 查询 GL 字符串）。在 booted iOS 26.5 模拟器实测 smoke 输出 `renderer='ANGLE (Metal Renderer: Apple iOS simulator GPU)'`、`version='OpenGL ES 3.0.0 (ANGLE 2.1.0...)'`——即 **GLES3 跑在模拟器硬件 Metal 上**，整条路线的前提被验证（此前模拟器 GLES 是软件栅格化，是 Snakes ~38 FPS 上限的根因）。打通两个坑并固化到 fetch 脚本/CMake：①上游 arm64 *simulator* slice 用旧的 `LC_VERSION_MIN_IPHONEOS` load command，modern dyld 拒载——`vtool -set-build-version 7`（iOS-Simulator）改写；②嵌入 framework 需给 app 加 `@executable_path/Frameworks` rpath。device slice（ios-arm64）已打包，真机启动验证留到 Phase 4。EAGL 路径默认不变。详见 [`ios_metal_angle_plan.md`](./ios_metal_angle_plan.md)。 |
| 2026-06-15 | **确认 Snakes 在模拟器里是「软件 GLES 渲染受限」而非 dispatch 受限；落地 LDM/STM 访存优化，规划 Metal（ANGLE）路线**。重采样实测两线程同忙：guest 解释线程满载于 `InterpreterMainLoop`，渲染线程满载于模拟器**软件 GLES**（`gldRenderFillTriangles`/`GLRendererFloat`），双缓冲 present 让 guest 卡在 GPU fence → Snakes 钉死 ~38 FPS，任何 CPU 侧优化在模拟器都无法体现。据此：①**CMP+B_COND_THUMB 超级指令融合**——正确（差分 100 万例 + regression 8/8）但 A/B 实测中性（~38.8 vs ~38.3，噪声内；解释器耗时在指令*执行*即 LDM/STM+load/store，不在 dispatch），**已回退**。②**LDM/STM 块游标**（`armstate.h` `block_cursor` + `ReadMemory32Block`/`WriteMemory32Block`，接入 `LDM_INST`/`STM_INST`，提交 `b5f5437a5`）：寄存器列表的连续地址几乎总在同一 guest 页，原先逐字一次 dyncom-TLB 查找，现每段只解析一次宿主页指针、跨页才重解析，语义与 `ReadMemory32`/`WriteMemory32` 逐位一致；差分 100 万例（5 seed）+ regression 8/8 验证。模拟器里 FPS 中性（受软件 GLES 限），但削减了最热访存函数的真实 CPU 工作，真机（硬件 GLES）与 CPU-bound 应用应获益，故**保留**。结论：dyncom 在模拟器已近收益上限，提升 Snakes 帧率需硬件加速渲染（真机 / Metal 模拟器路径）或 JIT（iOS 禁用）。**下一步规划接入 Metal**：选定 **ANGLE（GLES→Metal）** 路线——保留整套 GLES command-list 后端，经 EGL 跑在 ANGLE 的 Metal 后端上，构建开关 + EAGL 回退，同时硬件加速模拟器与真机（Metal 在 iOS 模拟器走 Mac GPU，而 Apple 的模拟器 GLES 是软件）；分 5 个阶段（Phase 0 spike → 依赖 → ANGLE EGL 上下文 + CAMetalLayer → shader/特性对齐 → 测帧+真机验证翻默认）。详见 [`ios_snakes_perf.md`](./ios_snakes_perf.md) 与 [`ios_metal_angle_plan.md`](./ios_metal_angle_plan.md)。 |
| 2026-06-14 | **dyncom 解释器深度优化 stages 1-3（Snakes 游戏 ~30-32 → ~34 FPS，regression 8/8）**，承接前一条把帧推到纯 CPU-bound 之后。①**Stage 1a 取指走 TLB**：`ARMul_State::ReadCode` 是唯一绕过 dyncom TLB 每次走页表的访存口，且 `mmu_base::read_code` 从不 seed TLB（数据读都 seed）；改为 ReadCode 先查 TLB、read_code 像数据读一样 `set_tlb_page` 填充（TLB 缓存宿主页指针非指令字，SMC 仍由 make_dirty/imb_range 失效）——ReadCode self-time ~90→~22、`read_code`/`page_directory::get_pointer` 退出 profile。②**Stage 3a AddWithCarry 内联**：每条 ADD/ADC/SUB/SBC/RSB/RSC/CMP/CMN 都调它，移到 armsupp.h 内联，无符号和+进位映射到 host `__builtin_add_overflow`、溢出保留 int64 规范定义（语义逐位不变）——从 profile 独立叶子消失。③**Stage 2 block L1 缓存**：DISPATCH 每次 taken 分支查 `unordered_map<(asid|vpc),ptr>`，前置 2048-entry 直接映射 L1（index+比较，~32KB，asid 标记键跨进程共存，仅随 map flush 清空，ctor 初始化哨兵键）——正确性中性，热块跳过哈希；孤立收益在噪声内（查找本就按块摊销）但无害。Stage 5 指令融合经深度评估后**主动不做**（仅 ~3-5%、需往逐指令翻译流水线硬塞前瞻匹配+合成指令类型、且无解释器差分测试框架只能靠 regression 验证，风险/收益不划算）；Stage 4 移位特化同样推迟；iOS 禁 JIT 故解释器即上限。详见 [`dyncom_optimization_plan.md`](./dyncom_optimization_plan.md)。 |
| 2026-06-14 | **Snakes（`0x2000730F`）3D 游戏帧率 ~22-23 → ~30-32 FPS（iPhone 16 Pro 模拟器，dyncom，O3）**。`sample` 实测两处瓶颈：①dyncom 指令缓存抖动——`load_context` 在**每次** guest 线程上下文切换时 `clear_instruction_cache()`，而 Snakes 每帧大量 IPC（window server + FBS 字形栅格）= 频繁进程切换 → 热渲染循环反复重译，`decode_arm_instruction` 占 CPU 线程 ~17%。修法：指令缓存按 `(asid<<32)|vpc` 标记地址空间（`armstate.h`/`arm_dyncom_interpreter.cpp`），调度器进程切换时 `core::set_asid()`（启用原先注释掉的 hook，`scheduler.cpp`），不同进程的译码块共存、跨 IPC 存活，不再每次切换清空；译码缓冲不再每次复位故加 128MB bump-allocator 溢出保护，SMC/代码重载仍由 `imb_range` 覆盖；嵌入其它后端做 fallback 的 dyncom 解释器（拿不到 asid，flag 门控）保留旧"每次清空"行为，dynarmic/12l1r 不受影响。实测重采样 `decode_arm_instruction` 从 ~17% → **0 采样**。②模拟器软件 GLES——`submit_screen_frame` 的 present blit 在模拟器上由 host CPU 软件栅格化整个 native-Retina drawable（iPhone 16 Pro 3×），guest 线程 ~27-30% 阻塞在 `wait_for`。修法：`EmulatorViewController.swift` 仅在 `targetEnvironment(simulator)` 把渲染面 scale 限到 1.5×（真机不变、用真 GPU），软件填充量降约 4×；触控坐标同用 `renderScale` 保持对齐。验证：`scripts/ios_regression_test.sh` PASS=8/8（Final Battle + Calculator 无 guest 崩溃），Snakes 进游戏稳定 ~30-32 FPS（爆炸粒子特效/关卡文字过场瞬时回落）。修复后重新 profile：帧已是纯 **CPU-bound**（guest os_thread ~99% 在 `InterpreterMainLoop`，图形线程 ~80% 空闲）——~78% 是不可约的 ARM 解释（dispatch+ALU+寻址+CondPassed，只有 JIT 能结构性削减，但 dynarmic 崩 Calculator 暂不可用）、~9.5% 卡在 present `wait_for`、~12% guest 访存（数据 load/store 已走 dyncom 512-entry TLB 快路径，唯 `ReadCode` 取指绕过 TLB 每次走页表，是剩余有界可优化点）。曾试 present 双缓冲（两 fence slot 轮换让 guest 领先一帧）可再到 ~32-34 FPS，但因偶发一次 Snakes guest 自旋卡死（疑时序触发既有调度脆弱性）已**回退**，present 保持单缓冲。详见 [`ios_snakes_perf.md`](./ios_snakes_perf.md)。 |
| 2026-06-14 | iOS 启动参数新增 ROM code 选择：`-LaunchROMCode rm-320`（别名 `-LaunchROM` / `-LaunchDeviceCode` / `-LaunchDevice`）会在 SwiftUI 启动后按 installed devices 的 `firmwareCode` 选中目标设备并调用既有 `bootDeviceAtIndex` 完整初始化；同时传 `-LaunchAppUID` 时，目标 ROM boot 完成后再进入 `EmulatorView` 自动启动应用。`scripts/ios_regression_test.sh` 的直接 UID 启动路径已带 `-LaunchROMCode rm-320`，覆盖 N95 回归入口。验证（booted iPhone 16 Pro，已装 RM-588 + RM-320）：安装 Debug simulator build 后运行 `-LaunchROMCode rm-320 -LaunchAppUID 0x10005902`，`devices.yml` 中 `RM-320` 为 index 1，启动后 `config.yml` 为 `device: 1`，UI 进入 Nokia N95 Calculator EmulatorView；日志无 guest panic / crash / access violation。随后 `scripts/ios_regression_test.sh` 通过（8 PASS / 0 FAIL，Final Battle + Calculator）。 |
| 2026-06-14 | iOS 虚拟键盘支持长按：`VirtualKeypad` 的方向键、OK、数字键、软键、拨号/挂断键改为 `HoldableRawKey`，触摸开始发送 `submitRawKey(..., pressed: true)`，触摸结束或视图消失发送 `pressed: false`，保留原按压动画和触感反馈；同时补上 accessibility button/action，自动化点击继续走 `tapRawKey`。验证（booted iPhone 16 Pro）：`./scripts/build_ios.sh simulator` 通过；安装 Debug simulator build 后运行 `-LaunchROMCode rm-320 -LaunchAppUID 0x10005902`，Calculator 虚拟键盘全部按键出现在 UI target 列表，XcodeBuildMCP 对 `1` 执行 1200ms long press 成功，应用保持运行。 |
| 2026-06-14 | 修复 Final Battle（`0xA0003C62`）进游戏后约 1 分钟自动 `E32USER-CBase 46`（stray-signal）崩溃。根因：周期定时器在 guest 写好 request status（pending）但还没 `SetActive()` 的竞态窗口里就完成请求，完成后该 AO 缺 `active` 标志、`active_scheduler::has_ready_request` 漏判 → `WaitForAnyRequest` 拿到对不上就绪 AO 的信号 → panic 46（dump 显示所有 AO 全 `Active,Pending`；`signal_request` caller 经 `atos` 解析为 `timer_callback`；`notify_info::complete` pre-set flags 实测致命那次为 `pending` 且 `active` 未置位）。修法：`timer.cpp`/`timer.h` 把 `timer_callback` 的 `request_finish()` 换成 `timer::fire_or_defer()`——当目标 status 为 `pending && !active` 且请求线程未阻塞（`request_count()>=0`，即 guest 正运行于 issue→SetActive 之间）时，用 `schedule_event(100us)` 有界重排完成事件（上限 8 次）让 guest 跑到 `SetActive`；裸 `User::WaitForRequest`（永不 active 但等待时阻塞）不满足条件、立即完成无额外延迟；EKA1 按原逻辑。曾尝试扩展 `wait_for_any_request` 的 stray-absorb hack 覆盖 fast-SVC，导致菜单按键被吞死锁，已弃用。验证（N95/rm-320，Release）：FBattle 进入真实游戏内（牢房场景）、菜单与游戏内输入正常、100s+ 不再崩；控制 app Calculator UI 正常无回归；`./scripts/build_ios.sh simulator` 通过。详见 [`ios-final-battle-timer-stray.md`](./ios-final-battle-timer-stray.md)。 |
| 2026-06-14 | AppList 新增「只看已安装应用 / 显示系统应用」过滤与长按卸载。`rescanApps` 给每个 `EKA2L1AppEntry` 标 `system` 标志（沿用 Qt/Android 启发式：`land_drive == drive_z && uid < 0x10300000` 即 ROM 内置系统应用），SwiftUI 用 `@AppStorage("appListShowSystemApps")` 默认 false 过滤——首屏只显示用户自己安装的应用，ellipsis 菜单加「Show/Hide System Apps」开关。长按应用条目（grid/list 均接 `.contextMenu`）弹出红色「Uninstall」，仅对非系统应用显示；点按后 `confirmationDialog` 二次确认，确认走新增 bridge `uninstallApp(uid:)` → `IosEmulator.uninstallAppWithUID:`（按 app UID 取 `manager->package(uid,0)` 及其 augmentations，调 `uninstall_package` 删文件+注册），随后 `rescanApps` 刷新并显示绿色 banner。验证（N95/rm-320，booted iPhone 16 Pro）：默认 12 个已安装应用；菜单开「Show System Apps」后变 66（54 个 ROM 系统应用被默认隐藏）；长按 Final Battle → Uninstall → 确认 → 列表降到 11、Final Battle 消失、banner「Uninstalled Final Battle.」。`./scripts/build_ios.sh simulator` 通过。 |
| 2026-06-13 | iOS 虚拟键盘视觉重做并补齐电话/挂断键（`EmulatorView.swift` 的 `VirtualKeypad`）：原来是一堆 `.bordered` 方块按钮，现改为带 `.ultraThinMaterial` 圆角面板的统一布局——左侧导航簇（LSK/RSK 软键 + 绿色拨号键 `phone.fill` / 红色挂断键 `phone.down.fill` + 圆形十字方向盘含中心 `OK` 选择键），右侧仿真手机数字键盘（数字下方带 ABC/DEF… 字母、0 带 `+`）。新增 `KeyStyle`/`PressableStyle` 两个 `ButtonStyle` 提供按压缩放与配色（数字/软键半透明、拨号绿、挂断红），点击附带 `UIImpactFeedbackGenerator` 轻触感。键码沿用 Symbian 标准 scancode（拨号=`std_key_application_0` 0xB4、挂断=`std_key_application_1` 0xB5，与 Qt 默认 keybind 一致）。验证：`./scripts/build_ios.sh simulator` 通过；booted iPhone 16 Pro 启动 N95 Calculator 截图确认新键盘整齐渲染、布局适配竖屏宽度、绿/红电话键与字母数字键盘显示正常。 |
| 2026-06-13 | **N95 Calculator Options 菜单打开/关闭全程跑通，默认样式不回归**。补齐 EKA2L1 自带但被注释禁用的 icon HLE（`akn_icon_server`，`services/src/ui/icon/`）并启用：原 `retrieve_icon` 只建空 bitmap（且哨兵尺寸 `KMaxTInt` 会让 `do_white_fill` 越界 SIGBUS），现改为真正渲染——`produce_icon_rgba`：①矢量容器（`.mif` SVG/NVG）走 `mif_file`→`convert_svgb/nvg_to_svg`→**lunasvg** 渲染到目标尺寸；②栅格容器（`.mbm`）走 `mbm_file`+`epoc::convert_to_rgba8888`→最近邻缩放；再写入 EColor64K 色图 + EGray256 掩码。关键修回归点：(a) 裸文件名（`Calcsoft.mif`/`avkon2.mif`）按 AknIconServer 习惯在 `\resource\apps\`、`\resource\` 逐盘解析，否则运算符图标找不到容器而空白；(b) `KMaxTInt` 尺寸按宽度+宽高比推导并 clamp 4096；(c) **必须同时渲染 `.mbm` 栅格皮肤元素**，否则默认计算器按钮/背景纹理缺失变洗白（启用 HLE 会完全替换 guest AknIconServer，无法只拦截失败的 NVG 图标）。启用后 guest 软件 OpenVG 不再跑（其 32bpp-only draw-device 工厂对 64k 图标 `User::Leave(KErrNotSupported)` 正是菜单不可见根因；真机走硬件 NVG，EKA2L1 无 GPU NVG）。验证（N95/rm-320）：默认态运算符(+−×÷±√%)与皮肤纹理/深蓝按钮背景与未启用 HLE 一致；按 Options 弹出菜单(`Last result`/Select/Cancel)，按 Cancel 关闭回主界面。遗留：mif 内嵌 bmp 类条目（如 `avkon2.mif` 某菜单项小图标）暂不渲染（透明，菜单项为纯文本不受影响）；CPU smoke test 偶发 spdlog 崩溃属既有无关问题。详见 [`ios-calculator-options-menu-hang.md`](./ios-calculator-options-menu-hang.md)。 |
| 2026-06-13 | 修复 Calculator 点 Options 软键 guest **卡死**：根因是 `bitmap_backed_canvas::execute_command`（`winuser.cpp`）的 `default` 分支对未实现 opcode 只 `LOG_ERROR` 不 `ctx.complete()`；AVKON 菜单的 backed-up fader 窗口同步发 `EWsWinOpEnableBackup (0x57)`，服务端不完成该同步 IPC → guest `SendReceive` 永久阻塞 → 整个 UI 死锁需重启。修法：`default` 补 `ctx.complete()`（对齐兄弟类 `redraw_msg_canvas`），并显式把 `EnableBackup` 当 no-op 成功处理。验证：修复后按 Options 不再出现 `0x57` 报错、log 不冻结、guest 回到正常 idle（非死锁）。**遗留**：死锁解除后菜单仍不可见——经 dump+反汇编逐级逆向到精确 ROM 指令：`AknIconServer` 渲染菜单图标时主动 `User::Leave(KErrNotSupported)`，源头是一个**只支持 32bpp 显示模式的 draw-device 工厂**（`cmp r0,#0xB; cmp r0,#0xC; bne→mvn r0,#4; bl User::Leave`，即只接受 EColor16MU/EColor16MA）。NVG 矢量图标渲染需要 32bpp 渲染面但拿到非 32bpp 模式。已排除 emulator 侧返回 -5（三处拦截全空）与屏幕模式（强制 color16ma 后图标 bitmap 仍标准 color64k+gray256、菜单仍不渲染）。真正修复=补齐 guest NVG 软件渲染的 32bpp draw-device 路径，属独立大型功能，疑 upstream 全局缺失。详见 [`ios-calculator-options-menu-hang.md`](./ios-calculator-options-menu-hang.md)。 |
| 2026-06-13 | 上一条死锁解除后，参考另一仓库 wasm 版对同一 Calculator-LSK 问题的诊断（根因 `FeatureManager::FeatureSupported(1012)`=`KFeatureIdAppMenuShowImages` 默认被拒 → `DynInitMenuPaneL` 删图片菜单项 → `DeleteMenuItem` panic EIKCOCTL 8），在 `featmgr.cpp` `do_feature_scanning` 把 1012 加入支持列表（EKA2L1 的 featreg.cfg 近乎空、默认 deny；真机视为支持）。**当前活动设备实为 N95（rm-320，device:0），非 5800**——N95 计算器不 panic（trace 无 panic），但加 1012 后菜单 pane 窗口确已创建（CW-DIAG），仍不可见：Calculator 在渲染 NVG 菜单图标时 leave（N95 上 FACTORY-DIAG 实测工厂入参 `r0=7=color64k`），pane 未被激活/绘制。**遗留**：N95 用软件 OpenVG（ROM 有 `102827cf.dll`+`libopenvg.dll`），其 NVG draw-device 工厂只接 32bpp，但图标 bitmap 是标准 color64k+gray256；运算符图标是 MBM 渲染正常、菜单图标是 NVG 失败。试过把 AknIconServer 的 64k bitmap 升级 16mu→图标能渲染但 MBM 颜色全错（64k 数据未转 32bpp）且破坏运算符外观；试过启用 EKA2L1 自带 icon HLE→`retrieve_icon→create_bitmap→do_white_fill` SIGBUS 崩溃且只出空图标。最小可行修复需要正确的 NVG/OpenVG 软件渲染（仅对 NVG 图标用 32bpp 且做格式转换），属独立功能。 |
| 2026-06-13 | 修复 iOS EmulatorView 从 Snakes 返回时 app 闪退：先修 `FPSOverlay` timer 中 `UInt64` 帧计数相减下溢（返回/重启时 C++ `rendered_frame_count` 重置为 0，触发 Swift arithmetic overflow / SIGTRAP）；随后定位到返回关闭 guest app 时 UI 线程直接 kill process，会与仍在 `symsys->loop()` 的 OS 线程并发，导致 `active_scheduler`/HLE 路径 native SIGSEGV。`closeRunningApp` 现在先置 `mounted=false`、唤醒 idle core、等待 `loop_mutex` drain 当前 tick，再在 kernel lock 下 kill guest process；EmulatorViewController 离屏时也显式 detach EAGL layer，避免继续向离屏 layer present。验证：`./scripts/build_ios.sh simulator` 通过；安装最终 build 后在当前 booted iPhone 16 Pro 启动 Snakes (`0x2000730F`)，快速点左上返回和等待约 20 秒后返回均回到 app list；截至 2026-06-13 10:49:28 未生成新 `EKA2L1` crash report，日志无 Snakes panic、Access violation、Corrupted graphics 或 Emulation halt。 |
| 2026-06-13 | EmulatorView 新增可拖动 FPS 浮层：iOS bridge 在每次 `submit_screen_frame` 后累计实际 guest 帧提交数，SwiftUI 每秒采样显示，默认位于游戏画面右上角；Settings/Input 新增 `FPS overlay` 开关。验证：`./scripts/build_ios.sh simulator` 通过；当前 booted iPhone 16 Pro 模拟器启动 Snakes (`0x2000730F`) 约 20 秒后进入主菜单，截图可见右上角 `8 FPS` 浮层；日志 56 行，无 Snakes panic、Access violation、Corrupted graphics 或 Emulation halt。 |
| 2026-06-13 | iOS guest 应用退出回弹细化：`launch_app` 的 process logon 现在把退出 type/category/reason 格式化为 fatal 详情；panic 或非 0 terminate 先在 EmulatorView 弹 `Guest fatal` alert，点 OK 后再关闭页面。普通 Exit 软键 / 正常退出仍直接回弹，返回键关闭路径仍先清 handler 再 `closeRunningApp`，不会弹出自杀式关闭的提示。 |
| 2026-06-06 | 修复上条"返回键关闭后第二次进入应用会自动关闭"的回归:根因是 SwiftUI `NavigationStack` 弹出 `UIViewControllerRepresentable` 时,UIKit 的 `isMovingFromParent`/`isBeingDismissed` 读到 `false`,导致返回键路径下 `viewWillDisappear` 从不触发 `closeRunningApp`——应用没被 kill、session 没标脏,第二次启动等于在同一 session 内再开一个实例并立即退出,把新 VC 一起带走。改为在 SwiftUI 层 `EmulatorView.onDisappear` 做"关闭即 kill"(它只在导航移除时触发,不受后台影响,后台仍走 scenePhase pause)。同时给每次启动加 generation token:reboot 拆旧 kernel 时被重新触发的旧应用 logon(`logon_requests_emu` 触发后不清理)按 generation 失配丢弃,不会误关当前 VC。N95 Calculator 验证:L1→返回→L2→返回→L3→Exit 软键→L4 全部正常渲染、退出回弹正确,无自动关闭。 |
| 2026-06-06 | iOS 模拟器 VC 与 guest 应用生命周期双向同步：关闭 EmulatorView（返回/dismiss）时通过 `closeRunningApp` 同步 kill 对应进程；guest 应用自行退出（Exit 软键 / 正常结束 / panic，均经 `launch_app` 的 `logon` 回调）时回弹关闭 VC。顺带修复**第二次打开应用白屏**：window/view server 在同一 booted session 内跨应用实例不会干净复位（guest 退出时 `Can't remove active view!`），导致 relaunch 渲染空白；参照桌面前端"退出即重启设备"的做法，在 `launchAppWithUID:` 检测到 session 脏（`needs_reboot_before_launch`）时先 `bootDeviceAtIndex:` 重建系统再启动，仅第二次及之后启动付出重建耗时。N95 Calculator 验证：首启渲染 → Exit 回弹 → 二启渲染 → 返回键 kill 回弹 → 三启渲染，全程无 panic/crash。 |
| 2026-06-05 | **已解决，待提交**：iOS 真机优化构建中 Snakes 的 `E32USER-CBase 46` 根因是同步 HLE sleep 将当前线程移出 ready queue 后缺少 `prepare_reschedule()`，guest CPU 继续执行等待态线程，timer `dewait()` 随后把仍在执行的线程改为 ready，最终破坏 `WaitForAnyRequest` 调度交接。`thread_scheduler::sleep(..., deque=true)` 现会立即请求 reschedule；调度器同时清理所属进程内存模型已经释放的 stale ready 线程，避免 guest panic 后的原生 SIGSEGV。RelWithDebInfo 真机 Snakes 95 秒和 Calculator 30 秒回归通过；Release/-O3 构建与安装成功，Snakes 95 秒回归通过。详见 [`ios-optimized-build-snakes-stray-signal.md`](./ios-optimized-build-snakes-stray-signal.md)。 |
| 2026-06-01 | 修复 N95 Snakes 启动画面卡死 / `E32USER-CBase 46` 误信号：请求信号被误消耗和补发导致调度失衡，修正请求等待与通知完成语义后，Snakes (`0x2000730F`) 可进入主菜单和实际游戏关卡，Calculator (`0x10005902`) 回归正常。详见 [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md)。 |
| 2026-05-30 | 定位并修复 N97（S60v5）ZipManager 在 iOS 上的**花屏（文字渲染成纯黑块）+ 触屏无效**两个 bug（均 iOS 端、桌面正常）。**花屏**两层 iOS GLES 模拟器特性：① 窗口服务给 AVKON 字形/图标 alpha mask（gray256 8bpp）建的 `R8`/`GL_RED` 纹理在 sim 上采样恒为 0（黑），数据上传无 GL error、桌面正常；16/24bpp（`GL_RGB`/`RGB565`，AVKON 渲文字进去的 backing bitmap）同样回黑。修法：iOS 下把 8/16/24bpp 一律提升为 `RGBA8`，上传时把源数据按位展开成 RGBA（8bpp→`(v,v,v,v)`、565 解包、24bpp BGR→RGB），并跳过原 stricted swizzle（`graphics_driver_shared.cpp` 全 `#if IOS` 守护）。② 标题/软键/列表文字用较大字号，字形图集 `align(50*size,1024)` 在 3× 显示缩放下达 `5120×5120`，超过 sim `GL_MAX_TEXTURE_SIZE`(≈4096) → `glTexImage2D` `INVALID_VALUE` → 图集纹理不完整、采样 `(0,0,0,1)` → 白字被画成纯黑块。修法：driver 新增 `max_texture_size()`（OGL 后端查 `GL_MAX_TEXTURE_SIZE`），`font_atlas::draw_text` 把图集尺寸 clamp 到该上限（跨平台正确性改进，只在超限时生效）。**触屏**：`IosEmulator` 派发 pointer 时把 `raw_screen_pos_` 误设为 `true`，window server 直接拿设备物理像素当 guest 坐标用（不减 `absolute_pos`/不除 `logic_scale_factor`），每次点击都落在 360×640 guest 屏之外 → 无反应；改为 `false`（与 Qt/Android 一致），由 window server 做 `absolute_pos + logic scale` 映射。验证：N97 ZipManager 文字（Zip manager / C: / (no data) / Options / Exit）+ 图标 + 滚动条全部正常渲染、零 GL error；N95 Calculator 无回归。触屏修复因本环境无法注入合成触控（Simulator 需 Accessibility 授权）只做了代码级验证（与 Qt/Android 逐字一致）。 |
| 2026-05-30 | 定位并修复 N97（S60v5）点 Calculator "黑屏 + CPU 100%" 的**日志洪水主因**：iOS 前端从不调 `parse_filter_string`，且非 `BUILD_FOR_USER` 构建默认 `*:trace`，叠加 spdlog `flush_on(debug)`，每次上下文切换 + 每条 dyncom VFP 子操作都被同步刷盘（N97 启动 6s 打 ~43 万行），CPU 全耗在 I/O。`IosEmulator` 启动时镜像 `BUILD_FOR_USER` 降级逻辑应用 normal-use preset + iOS 追加 `CPU*:warn`，settings 改 `logFilter` 即时重应用；修复后日志降到 ~220 行、VFP/scheduler flood 归零（详见阶段 3 修复清单 #5）。**但 N97 Calculator 仍黑屏**：剩余阻塞是 S60v5 AVKON FEP/PtiEngine（Zi 预测输入）`Can't open object: ZIDATA_/ZiAutoCh_` → Calculator 线程 fault（dyncom 0x10000 / dynarmic 0x30），属 backend/iOS 无关的 S60v5 仿真缺口，已记入阶段 3 已知风险（含 avkonfep 对 ROM XIP 文件覆盖无效、应走 `.map` patch 的线索）。期间另确认：sim 切 dynarmic 已不复现历史 regalloc crash、N95 Calculator dynarmic 下完整渲染，但因不解决 N97 且反转既有 dyncom 决定，未并入本次改动。 |
| 2026-05-24 | iOS 实现层代码熵清理：Swift target 切到 Swift 6；新增 `EKA2L1Bridge.swift` 作为 Swift facade，SwiftUI / UIKit Swift 控制器不再直接散落调用 ObjC facade；`EmulatorViewController` 从 ObjC++ 迁到 Swift，ObjC/ObjC++ 仅保留 `StartupBridge` / `CpuSmokeBridge` / `IosEmulator` 这类 C++ 桥接边界；bridging header 移除 UIKit 控制器导入，`IosEmulator.h` 补 `NS_SWIFT_NAME` 稳定 Swift API。回归验证：`scripts/build_ios.sh simulator` / `scripts/build_ios.sh smoke` 通过；xcodebuildmcp 复验 Calculator 与 Final Battle。 |
| 2026-05-24 | 推进阶段 3.8–3.11：iOS HWRM 振动接 Core Haptics；EmulatorView 增加多指/长按/pinch 与虚拟 S60 键盘，数字键对齐 Android 的 ASCII raw key；xcodebuildmcp 验证 Final Battle 语言菜单按虚拟键 `1` 可进入主菜单；新增 SwiftUI SettingsView 并通过 `IosEmulator` snapshot/apply 序列化到 `config.yml`；iOS input dialog no-op 替换为 UIAlertController 实现。 |
| 2026-05-24 | iOS simulator 路径回接 FFmpeg：新增 out-of-tree FFmpeg iOS 构建脚本，`build_ios.sh` 自动先产出 simulator/device 对应 static libs；CMake 在 iOS 下恢复 `ffmpeg` target，drivers 重新编入 DSP/audio/video FFmpeg backend。`scripts/build_ios.sh simulator` 通过，符号检查确认 `dsp_output_stream_ffmpeg` / `player_ffmpeg` 已进入 `libdrivers.a`。 |
| 2026-05-24 | 修复 iOS AppList 滚动到空格 caption 应用时的 `pystr::rstrip()` hardening abort，并给 MIF icon cache name 加 UID fallback 与图标解码串行化；修复 Final Battle 黑屏的直接原因：iOS DSP out stream 因 FFmpeg skip 返回空，当前先接 PCM16/PCM8-only fallback，游戏可进入语言选择界面。文档新增 3.7.1 iOS DSP / FFmpeg 回接计划。 |
| 2026-05-20 | 初版：拆完阶段 0，其余阶段仅列目标 |
| 2026-05-20 | 阶段 0 全部子任务（0.1–0.7）落地：iOS toolchain、emu 分支化、drivers / cpu 移除桌面/JIT 依赖、外部库审计、SwiftUI 骨架、构建脚本。等待 submodule init 后跑实际构建确认。 |
| 2026-05-20 | submodule init 后实际跑 `scripts/build_ios.sh device` 通过，产物：arm64 iOS `EKA2L1.app`。期间打掉 12 个真实编译/链接问题（见 0.7 子任务）。capstone / fmt 留下 dirty submodule patch 待后续 fork 收尾。 |
| 2026-05-20 | 升级 capstone → 5.0.7（换上游 `capstone-engine` 仓库）、spdlog → 1.17.0、fmt → 11.2.0；删除两个子模块 patch 和 `FMT_USE_CONSTEVAL=0` workaround，iOS device build 仍然通过。 |
| 2026-05-20 | 阶段 0 验收：`scripts/build_ios.sh simulator` 通过（arm64 iphonesimulator .app），在 booted iPhone 16 Pro (iOS 26.5) 上 `simctl install + launch` 成功，进程长时驻留无即时崩溃；`otool -L` 确认 device/simulator 产物均未链接 SDL2。阶段 0 七项验收标准全部打勾。 |
| 2026-05-20 | 阶段 1 拆解：缩窄为"dyncom 解释器跑通裸 ARM 片段"，dynarmic / MAP_JIT / entitlement 与发布通道合并到新的阶段 4。子任务 1.1–1.7 落地：smoke blob 设计、SmokeBridge、cpu iOS 真实可链、SwiftUI 展示、factory 回落语义、`build_ios.sh smoke` 验证、文档与遗留项。 |
| 2026-05-21 | 阶段 1 完成：simulator 上 `scripts/build_ios.sh smoke` exit 0，`EKA2L1_SMOKE: PASS backend=dyncom instrs=9 pc=0x00001024`。期间踩了 5 个坑（详见阶段 1 修复清单）。dynarmic 请求按预期回落 dyncom，UI 显示 fallback reason。 |
| 2026-05-21 | 阶段 2 拆解：目标聚焦"ROM 加载 → applist → 出一帧 + 单指交互"，音频/振动/导入 UI 全部推迟到阶段 3。子任务 2.1–2.11 落地：ogl 后端 iOS 化、EAGL 上下文、iOS emu_window、IosEmulator state、Documents 布局、applist 扫描、SwiftUI 三屏、触控、生命周期、`seed_ios_simulator_documents.sh`、文档收口。验证素材选定 `roms/N95 8GB (S60v3 - FP1)` + `roms/snakes-n95_n6trsohu.sis`。 |
| 2026-05-22 | 阶段 3.1 完成：iOS sandbox 下 `prot_read_write_exec` 被 mprotect 静默剥 W 导致 dispatcher trampoline chunk 写 0 SIGBUS。在 `translate_protection` iOS 分支统一剥 PROT_EXEC（dyncom 不需要 host exec），mount N95 ROM 后 applist 出 18 个应用，进程稳定。截屏归档 `docs/screenshots/ios-stage3/3.1-mount-unblocked/`。 |
| 2026-05-22 | 阶段 3.2 部分推进：在 `physical_file_system::get_real_physical_path` iOS 分支末尾加 case-insensitive 路径解析，修掉了 ROM 内混合大小写文件（首发证据是 `Wsini.ini`）在 iOS sandbox 下读不到导致的 `Loading wsini file broke with code -1`。剩余阻塞：点击 GUI app 后 guest "Main" 线程进 PC=0 access-violation 死循环（详见 3.2.1）—— stage 2 acceptance（render 真帧 + 1+1= + SIS 安装）在 3.2.1 落地前无法关闭，stage 2 状态仍为 🟡。 |
| 2026-05-22 | 阶段 3.2.1 调查暂停（待跨系统对比）：在 `process::create_prim_thread` / `scheduler::switch_context` / `cpu_exception_handler` 加 iOS 诊断 log + xcodebuildmcp 自动化，定位出 PC=0 直接来源是 caller `PUSH {r4,r5,r6,lr}` 后某个 BL 跳到 ROM 中段（`0x803A9F88` 处的 `POP {r4,pc}`），把栈上 `r5=0` 当作返回地址弹给 PC；dyncom block translator 把后续 0 字节解成 NON_BRANCH `ANDEQ r0,r0,r0`，于是 fault 沿 page 扫描而不是真死循环。继续推进需要桌面 Qt 同 ROM 对比或加 BL 追踪 —— 用户指示本轮在此暂停。 |
| 2026-05-23 | 阶段 3.2.1 关闭（host-page 对齐 root cause）：在 Apple Silicon Mac 上做原生 arm64 桌面 Qt 编译复现，加 `common::commit` mprotect 诊断后抓到 `[commit-fail] size=0x1000 errno=22 EINVAL`。结论：Apple Silicon host page = 16 KB，EKA2L1 用 4 KB 模拟页粒度调 `mprotect`，长度非 host page 倍数被 kernel 静默拒绝；guest 写未真提权的页就 SIGBUS / KERN_PROTECTION_FAILURE，被 KERN-EXEC=3 杀线程。修法（`779061f27`）：iOS / macOS arm64 下 `common::commit / change_protection` 调 mprotect 前把 ptr 向下、size 向上对齐到 `sysconf(_SC_PAGESIZE)`；`decommit` 反向对齐只 PROT_NONE 完全覆盖的 host 页。同步把 macOS arm64 加入 `translate_protection` PROT_EXEC strip。macOS arm64 上 ZipManager 已可正常进入。 |
| 2026-05-23 | 阶段 3.2 验收素材调整：验收应用恢复为 Calculator，后续不再用较难定位的 ZipManager 做 iOS 验收；SIS 测试样本从 `snakes-n95_n6trsohu.sis` 换成 `The Final Battle.sis`，原因是 Snake 自身的 codeseg 兼容性问题与移植无关。 |
| 2026-05-23 | 阶段 3.2.3 关闭：SIS payload 未写入 drive_e 的根因是 `sis_script_interpreter` 在 case-sensitive 平台把 VFS 解析后的 host 绝对路径整体 lower-case，iOS simulator container 里的 `/Users/.../Library/...` 被改成不存在的 `/users/.../library/...`。修法是只 lower-case Symbian 虚拟路径再解析 host path，并在 `get_raw_path` 失败时返回 false；复测 The Final Battle.sis 后 `drives/e/resource/apps/fbattle.rsc`、`drives/e/sys/bin/fbattle.exe`、`drives/e/private/10003a3f/import/apps/fbattle_reg.rsc` 均存在，applist 出现 `Final Battle, uid=0xA0003C62`。 |
| 2026-05-23 | 新增 macOS arm64 原生构建管线（`9103c2bca`）：homebrew ffmpeg@5 替换 x86_64-only 自带静态库（仅在 macOS arm64 走，未触碰 ffmpeg 子模块）；放开 `CMAKE_OSX_DEPLOYMENT_TARGET` 让 Qt6 ≥ 10.15；新增 `cmake/MacOSFinalizeBundle.cmake` post-build：修复 SDL2.framework symlink 布局、跑 macdeployqt、ad-hoc 重签，避免 Apple Silicon 拒绝加载未签 / 签名失效的 .app。 |
| 2026-05-23 | 阶段 3.2 推进：iOS sim 上 mount N95 → applist 62 app 正常 ✅。修 `gl_context_eagl::attach_layer` 把 CAEAGLLayer 的 opaque / drawableProperties / renderbufferStorage:fromDrawable: 三处 UIKit 调用全 dispatch_sync 到主线程（提交 `b9e92897b`），跑 Calculator 不再有 UIKit assertion，EAGL renderbuffer 正确分配。同时发现两个独立的剩余 blocker：①3.2.2 dyncom 在 iOS 上跑同一个 N95 Calculator 仍出 `PC=0 / LR=0x803A9F89` 的 BL→mid-function POP 现场；在 macOS arm64 桌面 build 上把 cpu 强制改成 dyncom 也完全复现，说明这是 dyncom 自己的 Thumb 解码 bug（macOS arm64 默认走 dynarmic 所以掩盖了）。②3.2.3 SIS 安装 The Final Battle.sis 时 sisregistry 元数据写成功但 payload 文件没落到 drive_e，applist 重扫 0 增量。两个 blocker 分别建为 3.2.2 / 3.2.3 子任务，3.2 验收暂停在"渲染真帧 + SIS 安装"前，mount + applist 部分可以归档为 3.2.A。 |
| 2026-05-23 | 阶段 3.2 继续：iOS simulator 放开并默认使用 Dynarmic，device 仍保留 dyncom；iOS 启动时复制 shader/patch 资源并按 Android 同款逻辑覆盖 `goommonitor.dll` / `avkonfep.dll`；补齐 `process_open_by_id` 特殊句柄；修正 scripting 关闭时 stale `ENABLE_SCRIPTING=1` 的 CMake 污染。xcodebuildmcp `build-and-run` 通过，The Final Battle.sis 安装闭环已通；Calculator / Notes 仍在 `_E32Startup` 早期返回到 `PC=0 / LR=0x803A9F89`，3.2 / stage-2 验收剩余 blocker 收窄为 3.2.2。 |
| 2026-05-23 | 阶段 3.2 继续：N95 `devices.yml` 由 ROM/Z 盘 metadata 生成，并对 `Series60v3.1.sis` 做 `epoc93fp1` 校正；iOS mount 不再等待 EAGL layer，Calculator 进入 EmulatorView 后等待 graphics driver 并绑定到 `symsys/winserv`。修复 iOS 无 audio/driver 下的 KeySound、Window redraw、graphics IPC 空指针崩溃；iOS simulator 默认回 dyncom，Dynarmic 保留构建但暂不默认。xcodebuildmcp 验证 mount N95 → Apps(63) → Calculator：无新 crash，Calculator 线程持续调度，剩余 blocker 从 PC=0/host crash 收敛为 EAGL 洋红画面，尚未刷出真实 Calculator UI，stage-2 acceptance 仍未关闭。 |
| 2026-05-22 | 阶段 3 拆解：把阶段 2 验收最后一公里（kernel chunk SIGBUS）并入阶段 3 作为 3.1 解锁项，3.2 补完 stage-2 acceptance，3.3 沉淀 `common::virtualmem` 非可执行内存 API。其余 3.4–3.13 覆盖真正的 ROM 安装流程、UIDocumentPicker 导入、SVG/MIF 图标、cubeb AudioUnit、Core Haptics、多指/手势、设置面板、UIAlertController 输入、字体引导、文档收口。dynarmic / MAP_JIT / 发布通道继续保留在阶段 4。阶段总览表把阶段 2 标为 🟡（待 3.1/3.2 翻 ✅），阶段 3 进入 🟡。 | |
| 2026-05-22 | 阶段 2 子任务 2.1–2.11 全部实现并 `scripts/build_ios.sh smoke` 双绿（simulator build PASS + 启动后 `EKA2L1_SMOKE: PASS backend=dyncom instrs=9 pc=0x00001024`）。期间打掉 10 个真实编译/链接/线程坑（见阶段 2 修复清单）。stage-2 的"实机加载 ROM、应用列表 ≥5、出一帧、触控触达 Calculator"等 acceptance 标准需要在 device 上 + 一个已 desktop-预装的 device 树才能完整跑通，落地与 stage-3 真实 ROM 安装流程一起做。 |
| 2026-05-23 (evening) | **阶段 2 验收关闭**。修掉 3.2.2 整屏洋红 root cause：`ogl_graphics_driver::bind_swapchain_framebuf()` 与 `bind_framebuffer(handle=0)` 硬编码 `glBindFramebuffer(GL_FRAMEBUFFER, 0)` —— iOS GLES 没有默认 framebuffer，FBO 0 是无效目标，渲染命令静默丢弃，EAGL drawable 保留 `renderbufferStorage:fromDrawable:` 之后的未初始化页面（Apple 用洋红做未初始化指示色）。新增 `gl_context::swapchain_framebuffer()` 虚函数（桌面默认 0），iOS 的 `gl_context_eagl` 返回内部 m_framebuffer；ogl 后端两处 FBO 0 改成 `context_->swapchain_framebuffer()`。同步把 `IosEmulator::submit_screen_frame` 的反向 `present_status` 逻辑改回 Qt/Android 同款。xcodebuildmcp 自动化验证：iPhone 16 Pro / iOS 26.5 模拟器上 build-and-run → mount N95 → applist 63 → tap Calculator → Calculator 真实 UI（+ - × ÷ = ± √ % ↑↑ ↓↓ / Options / Exit）稳定 ≥12s，无 crash；tap The Final Battle.sis 安装 → applist 出 `Final Battle, uid=0xA0003C62`；tap Final Battle → EmulatorView 切到 FBattle 进程（持续调度，音频缺失导致 hssSDthread 退出但符合"游戏内逻辑跑不下去也算通过"的接受标准）。截屏归档 `docs/screenshots/ios-stage3/2-acceptance/{calculator-rendered,applist-with-final-battle,final-battle-launched}.jpg`。阶段总览表把阶段 2 翻 ✅。剩余 follow-up：3.11 虚拟键盘后才能在 Calculator 上做 `1 + 1 =`（S60v3 数字键没有 on-screen 按钮）。 |
| 2026-06-16 | iOS 部署目标从 18.0 降到 16.0：`build_ios.sh` / `build_ios_ffmpeg.sh` 默认 `EKA2L1_IOS_DEPLOYMENT_TARGET` 改 16.0，并由 `build_ios.sh` export 该值给 FFmpeg 子构建（消除 ld 的 18.0/16.0 min-version 不匹配警告）。SwiftUI 侧两个 iOS 17-only API 做向下兼容：`ContentUnavailableView`（ContentView 的 bootError / emptyState 两处）加 `#available(iOS 17)` 分支，iOS 16 走新增的 `FallbackUnavailableView`（icon+title+desc+action 居中布局）；`.onChange(of:){ _, new in }` 两参数闭包（EKA2L1App scenePhase、EmulatorView proxy.size）改回 iOS 16 单参数形式。清掉 FFmpeg 缓存产物后 `scripts/build_ios.sh simulator` Release 重建 BUILD SUCCEEDED 且无 min-version 警告；产物 target triple `arm64-apple-ios16.0-simulator`。注：iOS 16 fallback 分支只能在真正的 iOS 16 设备/模拟器上目视验证（当前 booted sim 为 iOS 17+，#available 仍走新控件）。 |
