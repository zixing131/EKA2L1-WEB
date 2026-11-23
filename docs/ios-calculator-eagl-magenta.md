# iOS Calculator 已启动但 EAGL 仍无真帧（整屏洋红）

> 来源：阶段 3.2.2（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决。
>
> **一句话结论**：洋红空帧的 root cause 是 ogl 后端 `bind_swapchain_framebuf()` / `bind_framebuffer(0)` 硬编码 `glBindFramebuffer(GL_FRAMEBUFFER, 0)`，而 iOS GLES 没有默认 framebuffer（FBO 0 无效、渲染命令静默丢弃，drawable 保留 Apple 的洋红未初始化色）；修法是给 `gl_context` 加 `virtual swapchain_framebuffer()`、由 `gl_context_eagl` 返回挂着 CAEAGLLayer colorbuffer 的内部 FBO，桌面/Android 仍返回 0。期间还连修了三类缺省 driver 空指针崩溃 + present 时序对齐。

- 现象：host-page 对齐修复（`779061f27`）+ EAGL 主线程修复（`b9e92897b`）+ iOS sim Dynarmic backend + ROM patch 复制/覆盖落地后，iOS 模拟器上点 Calculator：
  - **没有** `[commit-fail]` —— mprotect 全部成功（3.2.1 unblocked）。
  - **没有** UIKit assertion —— renderbuffer 正确 attach（3.2 render 路径 unblocked）。
  - **仍然** 出现 `Access violation reading address 0x0 in thread Main (pc=0x00000000 lr=0x803A9F89 process=Calcsoft[10005902]0001)`；Notes 也复现同一 `LR=0x803A9F89`，因此不是 Calculator 单点问题。
- 新增结论：iOS sim 已启用 Dynarmic，`launchAppWithUID` 日志确认 backend=Dynarmic，但 Calculator / Notes 仍在 `_E32Startup` 早期清理路径返回到初始 stack metadata。SVC trace 显示退出前只有 `dll_tls(0x4E)` → `handle_close(0x6A)` → `library_detached(0xA2)` → `process_open_by_id(0x72)`；补齐 `process_open_by_id(0xFFFF8001 /* crr_thread */)` 后 `r0` 从 `KErrNotFound` 变成有效 handle，但 app 仍继续返回并 `POP {r4,pc}` 到 metadata 里的 `1`，最终 PC=0。说明剩余 blocker 更像 app startup / cleanup 语义缺口，而不是 EAGL、ROM patch 或 dyncom 独立解码问题。
- 当前保留修复：
  1. iOS simulator 允许构建并默认使用 Dynarmic；device 仍保留 dyncom，等 stage 4 entitlement / MAP_JIT。
  2. iOS 启动时复制 shader + patch 文件，并在 mount 后应用 Android 同款 ROM DLL 覆盖。
  3. `process_open_by_id` 支持 `special_handle_type::crr_process / crr_thread`，避免把特殊句柄误当普通 process id。
  4. CMake 在 scripting 关闭时显式写 `ENABLE_SCRIPTING=0`，避免源码树里的 `configure.h` 被 macOS 配置污染后让 iOS target include 缺失。
- 下一步选项（按代价从低到高）：
  1. 对照 macOS arm64 Dynarmic 跑 **Calculator**（不是 ZipManager），确认同一 ROM 下内建 GUI app 是否也走 `_E32Startup` early cleanup。
  2. 给 `library_detached / process_open_by_id / thread/process startup metadata` 增加最小单元复现，确认 EKA2 process startup 初始 stack metadata 与 S60v3 euser 期望是否匹配。
  3. 若 macOS Calculator 正常，再继续收窄 iOS frontend 初始化差异（audio/sensor/null driver、AppArc/window-server startup 时序、launch command line）。
- 验收：选定方案 → iOS sim 上 Calculator 启动后 EmulatorView 出非黑像素 → 单指 tap `1 + 1 =` 可见结果 → 截屏归档 `docs/screenshots/ios-stage3/3.2.2-render/`。
- **当前结论**：3.2.2 是 stage 3 范围内一个独立的真 bug，也是 3.2 / stage-2 验收最后的功能 blocker。
- **最新进展（2026-05-23）**：回退 iOS 默认 CPU backend 到 dyncom（simulator 仍保留 Dynarmic 可构建，用于后续定点调试；Dynarmic 当前在 Calculator A32 memory emit/regalloc 路径 host crash），并连续修掉三类 iOS frontend 缺省 driver 崩溃：①Window Server redraw 在 graphics driver 尚未发布时空指针；②KeySound server 在无 audio driver 时解引用空指针；③`bitmap_cache::add_or_get → drivers::create_bitmap` 在空 driver 下进入 graphics IPC。随后调整 mount/launch 时序：mount 不再硬等 EAGL layer，进入 EmulatorView 后等待 driver 并注入 `symsys/winserv`。xcodebuildmcp 复测：mount N95 → Apps(63) → Calculator，进程持续运行且无新 `.ips`，日志确认 `Calculator[10005902]` 线程持续调度；剩余问题是 EAGL 画面为整屏洋红，虽然 FBO incomplete 已通过 iOS color-only bitmap FBO / 32bpp RGBA render-target 收敛，但最终 screen_texture/present 仍未刷出 Calculator UI。
- **关闭（2026-05-23 evening）**：洋红画面的 root cause 是 `ogl_graphics_driver::bind_swapchain_framebuf()` / `bind_framebuffer(handle=0)` 都硬编码 `glBindFramebuffer(GL_FRAMEBUFFER, 0)` —— iOS GLES 没有默认 framebuffer，FBO 0 是无效目标，所有渲染命令静默丢弃，EAGL drawable 上的 renderbuffer 一直保留 `renderbufferStorage:fromDrawable:` 之后的未初始化内存（Apple 用洋红做未初始化指示色），从外观上看就是"已 attach、已 present、但画面是 magenta 清屏"。修法：① `gl_context` 基类新增 `virtual unsigned int swapchain_framebuffer() const { return 0; }`；②`gl_context_eagl` override 返回内部 `m_framebuffer`（attach_layer 已经把 CAEAGLLayer 的 colorbuffer 挂到这个 FBO 的 color attachment 0 + depth/stencil renderbuffer）；③ogl 后端的两处 "FBO 0" 改成 `context_->swapchain_framebuffer()`，桌面 / Android 行为不变（仍然返回 0 = system default framebuffer）。同时把 `IosEmulator::submit_screen_frame` 里那段把 `present_status` 从 -100 强行复位 0 的反向 wait_for 改成与 Qt / Android 一致的 `wait_for → set -100 → present`。验证：xcodebuildmcp `build-and-run` → tap N95 → Mount → applist 63 → tap Calculator → screenshot **`docs/screenshots/ios-stage3/2-acceptance/calculator-rendered.jpg`** 显示真实 Calculator UI（+ - × ÷ = ± √ % ↑↑ ↓↓ / "Options" / "Exit" 软键 / 灰色显示区），稳定 ≥12s 无 crash。Final Battle launch 同样进入 EmulatorView，FBattle 进程持续调度（hssSDthread 因无 audio driver 报 `Unable to create new DSP out stream` 然后 access violation 杀线程，但符合 stage 2 "(游戏内逻辑跑不下去也算通过)" 接受标准；音频在 3.7 上线后会解掉）。
