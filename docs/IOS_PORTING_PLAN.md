# EKA2L1 iOS 移植方案

> **已归档（Frozen），不再更新。** 保留作为移植初期方案的历史记录；当前状态见 [`IOS_PORTING_TASKS.md`](./IOS_PORTING_TASKS.md) 与 [`README.md`](./README.md) 索引。
>
> 目标：让 EKA2L1（Symbian / N-Gage 模拟器）在 iOS / iPadOS 上跑起来。
> 本文基于当前仓库 `master`/`ios` 分支的源码结构整理，给出可执行的落地路径，而非一次性 PR 清单。

---

## 1. 现状速览

EKA2L1 的代码组织：

- 内核 / 系统层（`src/emu/{kernel,system,services,mem,vfs,loader,...}`）：纯 C++17/20，绝大部分 POSIX 兼容，理论上对 iOS 透明。
- CPU 层（`src/emu/cpu`）：
  - 64 位主机：使用 **dynarmic** JIT（ARM 客户机 → 主机汇编）。
  - 32 位主机：使用自研 `12l1r` JIT。
  - 还存在 `dyncom` 解释器作为兜底。
- 图形驱动（`src/emu/drivers/src/graphics/backend/`）：
  - 仅 OpenGL/GLES 后端可用（Vulkan 后端有桩但默认关闭）。
  - 上下文实现：WGL（Win）、AGL（macOS）、GLX/EGL/Wayland（Linux）、EGL+Android（Android）。**iOS 尚未存在**。
- 前端 UI：
  - 桌面：Qt6（`src/emu/qt`）。
  - Android：`src/emu/android/`（Kotlin/Java）+ `src/emu/android/app/src/main/cpp/`（JNI 桥 `launcher` / `state` / `emu_window_android` / `thread` / `input_dialog`）。
  - **iOS 尚未存在**。
- 外部依赖（`src/external/`）：dynarmic、ffmpeg、cubeb、SDL2、freetype、capstone、luajit、libuv、mbedtls、miniBAE、TinySoundFont、lunasvg、yaml-cpp 等；其中 SDL2 仅用于桌面输入/振动，Android 已绕开。
- 构建系统：CMake 顶层 + 各模块；针对 Android 在 `src/emu/CMakeLists.txt`、`src/emu/drivers/CMakeLists.txt`、`src/emu/android/app/src/main/cpp/CMakeLists.txt` 中分支处理。

---

## 2. iOS 上的关键阻塞点

| # | 问题 | 影响 | 应对策略 |
|---|------|------|----------|
| A | **JIT 限制**：iOS 普通应用沙箱无法 `mmap(... PROT_EXEC | PROT_WRITE ...)`，dynarmic / 12l1r 默认无法工作 | CPU 核心跑不起来 | 1) 提供 dyncom 解释器纯软件路径作为最低可用方案；2) 加入 Apple 的 `MAP_JIT` + `pthread_jit_write_protect_np` 双视图机制，配合 `com.apple.security.cs.allow-jit` entitlement，在带 JIT 权限的环境（开发者签名 + Xcode 调试 / AltStore + JIT Enabler / 越狱 / TrollStore-installd 通路）下启用 JIT |
| B | **OpenGL 上下文缺失** | 无法渲染 | 新增 `context_eagl.mm`：基于 `CAEAGLLayer` + `EAGLContext`（GLES2/3）。后续可考虑迁移到 Metal/MoltenVK |
| C | **前端 UI 缺失** | 无可启动入口 | 新增 `src/emu/ios/` 子工程：UIKit/SwiftUI 壳 + Objective-C++ 桥层，对应 Android 的 `launcher`/`state`/`thread`/`input_dialog` |
| D | **构建系统未配置 iOS** | 无法生成工程 | 引入 `ios-cmake` 工具链 + 新增 `if (IOS) ...` 分支；或额外维护一个 Xcode 工程文件，靠 CMake 生成 |
| E | **第三方库 iOS 可构建性** | 链接失败 | 见 §5：逐项审计 dynarmic / ffmpeg / cubeb / SDL2 / luajit / libuv / mbedtls / freetype |
| F | **沙箱文件系统** | 安装 SIS / 选择 ROM 受限 | 全部路径走 `Documents/` 容器；通过 `UIDocumentPickerViewController` 导入 ROM、SIS、字体；启用 `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace` 让用户用"文件" App 管理 |
| G | **音频/输入/振动** | 平台后端缺失 | 复用 cubeb 的 AudioUnit 后端（已存在）；振动用 `UIImpactFeedbackGenerator` 或 Core Haptics；触控通过新增 iOS 窗口层转成 `drivers::emu_window` 事件；省略 SDL2 |
| H | **luajit on iOS arm64** | 脚本系统编译/运行失败 | luajit 在 iOS 上需关闭 JIT（用解释器模式 INTERPRETER-only）；建议第一阶段直接 `EKA2L1_ENABLE_SCRIPTING_ABILITY=OFF` |
| I | **C++20 / libc++ 兼容** | 偶发编译失败 | 使用 Xcode 15+，最小部署 iOS 14+（保证 `<filesystem>`、C++20 特性）|

---

## 3. 推进路线（分阶段）

### 阶段 0：可构建（1–2 周）
- [ ] 引入 `cmake/ios.toolchain.cmake`（leetal/ios-cmake）。
- [ ] 顶层 CMakeLists：识别 `IOS`/`CMAKE_SYSTEM_NAME STREQUAL iOS`；默认关闭 `EKA2L1_BUILD_TESTS`、`EKA2L1_BUILD_TOOLS`、`EKA2L1_ENABLE_SCRIPTING_ABILITY`、`EKA2L1_BUILD_VULKAN_BACKEND`。
- [ ] `src/emu/CMakeLists.txt`：在 `if (ANDROID) ... else qt` 之外增加 `elseif(IOS) add_subdirectory(ios)` 分支。
- [ ] `src/emu/drivers/CMakeLists.txt`：iOS 分支不引入 SDL2，参考 Android 写法；新增空的 camera/sensor/vibration null 后端。
- [ ] 让 `common`、`mem`、`kernel`、`services`、`utils`、`vfs`、`loader`、`package`、`drivers` 至少能编译通过（不连接前端）。

### 阶段 1：CPU 跑通（解释器优先）
- [ ] 默认走 `dyncom` 解释器，验证 `arm_factory` 在 `IOS` 宏下能选择非 JIT 后端。
- [ ] 编译 dynarmic 子模块至 iOS arm64：补 `pthread_jit_write_protect_np` 路径；运行时检测 entitlement，无 JIT 权限自动回落 dyncom。
- [ ] 写一个 `Tests/cpu_smoke` 小程序用 XCTest 跑 EUSER 几条指令路径。

### 阶段 2：iOS 前端壳 + 渲染
- [ ] 新增目录 `src/emu/ios/`：
  - `App/` Swift（SwiftUI）：启动、首选项、ROM 选择、设备选择、应用列表。
  - `Bridge/` Objective-C++：对照 `src/emu/android/app/src/main/cpp/`，实现 `IOSLauncher`、`IOSState`、`IOSEmuWindow`、`IOSThread`、`IOSInputDialog`。
  - `Resources/`：预置 `compat/`、`patch/`、`resources/`、`scripts/`（同 Android 拷贝列表）。
- [ ] 新增 `src/emu/drivers/src/graphics/backend/context_eagl.{h,mm}`：实现 `context::make` 在 iOS 上返回 EAGL 上下文；驱动 `CAEAGLLayer`/`GLKView`/自管 framebuffer。
- [ ] 触控 → `emu_window` 指针事件；按键映射先用屏上虚拟按键（N-Gage 是关键场景）。
- [ ] 至少能加载一个不依赖音频的 demo（Symbian 自带 calculator/clock）。

#### iOS sandbox 目录布局（任务 2.5 定稿）

iOS App sandbox 的 `Documents/` 一律按下面的树状结构组织。`IosEmulator
::startWithDocumentsPath:` 在启动时若目录缺失则自动创建；emu 进程在
启动后 `chdir` 到 `Documents/data/`，使 `config::state::storage` 与底
层 IO 路径假设都落在该子目录内。

```
<App Sandbox>/Documents/
├── roms/                                ← 用户拷入的 ROM 解压目录
│   └── <rom-folder>/                    ← 一个 ROM 一个一级子目录
│       └── SYM.ROM                      ← ROM 入口文件（或 .img/.ROM 等）
├── data/                                ← eka2l1::system 的 data root
│   ├── drives/                          ← Symbian C/D/E/Z 持久化盘
│   │   ├── c/                           ← 内部存储
│   │   ├── d/                           ← 临时盘
│   │   ├── e/                           ← 可移动卡（应用安装）
│   │   └── z/                           ← ROM 镜像挂载点
│   ├── compat/                          ← 兼容补丁/脚本目录
│   ├── config.yml                       ← `conf.deserialize/serialize` 落地
│   └── EKA2L1.log                       ← spdlog 写入
└── sis/                                 ← 手工放入待安装的 .sis / .sisx
```

- 路径含空格（如 `N95 8GB (S60v3 - FP1)`）是常见情况；shell 脚本必须
  正确引用，C++ 侧不要在路径里强行 split。
- ROM 通过 Files App 或 `scripts/seed_ios_simulator_documents.sh`
  拖进 `Documents/roms/<name>/`；前端按一级子目录枚举。
- `.sis` 直接拷进 `Documents/sis/`，前端按文件名枚举（真正的
  `UIDocumentPickerViewController` 入口推迟到阶段 3）。
- 进入应用沙箱：`xcrun simctl get_app_container booted com.eka2l1.emulator data`
  返回的就是 `<App Sandbox>` 的 host 路径，可直接 `cp -R` 进 Documents。

### 阶段 3：完整体验
- [ ] 音频：cubeb AudioUnit / Core Audio 后端打通。
- [ ] 振动：`UIImpactFeedbackGenerator` 实现 `vibration` 接口。
- [ ] 文件导入：`UIDocumentPickerViewController` + `Info.plist` 文件关联（`.sis/.sisx/.n-gage/.rom`）。
- [ ] 字体导入引导（Symbian ROM 字体通常缺失，需用户提供）。
- [ ] 设置面板覆盖 `config::state` 主要字段（设备、屏幕方向、JIT 开关、上采样、声音、按键映射）。
- [ ] JIT 路径完善：检测 `csops`/entitlement，提示用户启用方式；带 fallback。

### 阶段 4：发布通道
- [ ] 双签策略：
  - 开发者证书 + AltStore / Sideloadly：JIT 通过 Xcode 调试附加。
  - TrollStore（iOS 14–16）：永久签名 + JIT entitlement。
  - 越狱：完整 JIT。
  - App Store：仅解释器模式（不可上架原始模拟器，但可作为"私人发行"参考）。
- [ ] 持续集成：`xcodebuild -scheme EKA2L1` + 模拟器烟测（仅解释器，模拟器没有 arm64 客户机意义，仅用于编译验证）。

---

## 4. 代码改动清单（粗粒度）

```
CMakeLists.txt                                      # 顶层加入 IOS 选项；默认关闭 tools/tests/scripting
cmake/ios.toolchain.cmake                           # 新增（或 vendored leetal/ios-cmake）
src/emu/CMakeLists.txt                              # add_subdirectory(ios) 分支
src/emu/ios/                                        # 新增整个目录
    CMakeLists.txt
    App/                                            # SwiftUI 工程
    Bridge/include/ios/{launcher,state,emu_window_ios,thread,input_dialog}.h
    Bridge/src/{launcher,state,emu_window_ios,thread,input_dialog}.mm
    Resources/                                      # Info.plist, entitlements, 资源拷贝
src/emu/drivers/CMakeLists.txt                      # if (IOS) 分支：不链 SDL2，加 EAGL 源
src/emu/drivers/src/graphics/backend/context_eagl.h
src/emu/drivers/src/graphics/backend/context_eagl.mm
src/emu/drivers/src/graphics/context.cpp            # 工厂里增加 iOS 分支
src/emu/drivers/src/audio/...                       # 确认 cubeb iOS 后端启用
src/emu/drivers/src/hwrm/backend/vibration_ios.{h,mm}
src/emu/drivers/src/sensor/backend/null/             # 复用
src/emu/cpu/src/arm_factory.cpp                     # iOS：默认 dyncom；JIT 可选
src/emu/common/src/virtualmem.cpp                   # 加 MAP_JIT 路径（dynarmic 用）
src/external/dynarmic                               # 上游或本地 patch：iOS JIT write protect
src/external/CMakeLists.txt                         # iOS 分支：跳过/替换 SDL2、调整 ffmpeg 配置
```

---

## 5. 第三方依赖审计

| 库 | iOS 状态 | 行动 |
|----|----------|------|
| dynarmic | 上游对 macOS arm64 支持完善，iOS 需补 `MAP_JIT` + write protect toggle 调用点 | 验证 fork（`EKA2L1/dynarmic`）；如缺，做 patch |
| ffmpeg | EKA2L1 fork 含 iOS 构建脚本？需确认 | 若无，加 `--target-os=darwin --arch=arm64 --enable-cross-compile` 配置 |
| cubeb | 自带 AudioUnit 后端 | 直接启用 |
| SDL2 | iOS 上可编译，但拖入 Cocoa Touch 依赖 | 第一阶段绕开，仿照 Android 不链 |
| luajit | iOS 上仅解释器模式 | 阶段 1 先关 scripting；后续如需，按 luajit iOS 指南编译 |
| libuv / mbedtls / freetype / capstone / yaml-cpp / re2 / xxHash / pugixml / lunasvg / fmt / spdlog / sqlite3 / stb / libfat / libtess2 / miniBAE / TinySoundFont / miniupnp | 纯 C/C++，预期可直出 | CMake 子目录跑通即可 |
| glad（GL 加载器） | iOS GLES 不需要 glad，符号直接来自 OpenGLES.framework | iOS 分支不链 glad，或重新生成 GLES2/3 loader |
| glm / ext-boost / thread-pool | header-only | 无 |

---

## 6. 风险与未决项

1. **JIT 政策**：没有 JIT 时纯解释器性能不足以跑 3D N-Gage 游戏；解释器吞吐如何，需早期 benchmark。如不可用，考虑：
   - 投资 12l1r 的 ARM64-host 后端（目前是 ARM32 host）。
   - 或迁移到 Apple Hypervisor.framework（仅 macOS，iOS 没有；可作 macCatalyst 备选）。
2. **OpenGL ES 在 iOS 已 deprecated**：仍可运行但未来不保证。Vulkan 后端只能借 MoltenVK，且 EKA2L1 的 Vulkan 后端尚不完整。中长期看需要 Metal 后端，是更大工程。
3. **App Store 政策**：含 JIT 或模拟商业系统的应用通常无法上架；定位明确为 sideload / TrollStore / 越狱发行。
4. **MacCatalyst 备选**：若 iOS JIT 路径不畅，可先用 MacCatalyst 跑通 Qt 或 SwiftUI 壳，复用 macOS dynarmic 路径，降低风险。
5. **字体/ROM 合法性**：依赖用户自带 Symbian ROM/字体，App 仅做加载，不分发。

---

## 7. 下一步建议（最先动手）

1. 把 `src/emu/CMakeLists.txt`、`src/emu/drivers/CMakeLists.txt` 改造成识别 `IOS` 的最小骨架，先让 `common` + `mem` + `cpu(dyncom only)` 编过 iOS arm64。
2. 同时新建 `src/emu/ios/` 空骨架，写一个最小 SwiftUI App，链接一个空的静态库聚合 target，验证整条链路。
3. 加 `context_eagl.mm` 草稿 + 一个能 `glClear` 的页面。
4. 之后再接 `system::startup` / `launcher` 流程，跑通最简单 EXE 加载。

每一步都建议作为独立 PR/commit，便于上游或下游回合并。
