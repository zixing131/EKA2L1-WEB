# app launch 后 guest "Main" 线程 PC=0 死循环

> 来源：阶段 3.2.1（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决（host-page 对齐后自动解决）。
>
> **一句话结论**：root cause 是 Apple Silicon/iOS 用 16 KB host page、但 memory model 按 4 KB 粒度调 `mprotect`，`len=0x1000` 不是 host page 整数倍被 kernel 静默 `EINVAL` 拒绝，留在 `PROT_NONE` 的页被写/弹栈即 SIGBUS、dyncom 继续往下解码零字节就表现为 "PC=0 沿 page 扫描"；修法 `779061f27` 在 `virtualmem.cpp` 的 commit / change_protection 里把范围对齐到 `sysconf(_SC_PAGESIZE)`，仅 iOS/macOS arm64 生效。

- 现象：mount N95 ROM 后从 SwiftUI 列表点任意 GUI app（已试 Themes uid 0x10005A32、Mce/Messaging uid 0x100058c5），IosEmulator 调到 `alserv->launch_app`，新进程的 local chunks（0x400000 / 0x500000 / 0x600000 / 0x700000）都创建成功；紧接着 dyncom 开始连续打 `Access violation reading address 0x0 / 0x4 / 0x8 / …` 直到当前 page 结束。EmulatorView 渲染面停在黑色。
- 通过加在 `process::create_prim_thread` / `thread_scheduler::switch_context` / `kernel_system::cpu_exception_handler` 的 iOS 诊断日志（带 `// TODO(ios)` 标签）+ xcodebuildmcp 自动化，拿到以下事实链：
  1. `create_prim_thread: process=Mce code_addr=0x82715718 ep_off=0x82715718 stack=0x10000` —— 入口地址来自 codeseg，是合法 ROM 地址（SYM.ROM 文件偏移 0x2715718 处确实是 ARM 指令）。
  2. `Switch to thread Main pc=0x82715718 lr=0x00000000 sp=0x0050FFC0 r4=0x00000000 cpsr=0x00000000` —— 调度器只调度了一次（整个 run 里 switch_context 计数 = 1）。
  3. 立即开始 access violation，但 `lr` 已经从 0 变成 `0x803A9F89`（ROM 内一段合法 Thumb 地址）。说明在调度切上来到第一次 fault 之间，guest 真的执行了若干 ARM/Thumb 指令并发生了 BL/BLX。
  4. 静态分析入口：`0x82715718` 的 ARM 入口 stub（TST/CMP/B/MOVLS/BLS）→ 跳到 `0x82724C68` 的 ARM→Thumb veneer（`ADD r12, PC, #1; BX r12`）→ Thumb 入口 `0x82724C70` (`PUSH {r4,r5,r6,lr}; MOVS r5, r0; MOVS r4, r1; BLX 0x8272699C`...)。`r5 = r0 = 0`, `r4 = r1 = sp`。
  5. 最终是某条 `BL` 跳到了 ROM 中段（`LR=0x803A9F89` 对应 ROM 中 `POP {r4, pc}` 的下一指令），callee 在没有先 `PUSH` 自己栈帧的情况下立刻 `POP {r4, pc}`，把 caller `PUSH {r4,r5,r6,lr}` 留下的 `r5` 值（=0）当作返回地址弹给 `PC` —— **PC=0 的直接来源就是 caller 栈帧上 `r5=0` 的槽**。
  6. fault 是连续 page 扫描而非死循环：dyncom block 翻译器对 `ReadCode → 0` 的失败返回 `0x00000000`，被 decode 成 `ANDEQ r0,r0,r0` (NON_BRANCH)，于是 `phys_addr += 4` 继续翻译直到 page boundary 0xFFF。所以一次"launch 失败"在日志里会看到约 1024 条 access violation。
- 候选 root cause（按可能性排序）：
  1. **codeseg / libmanager 的 import 解析 / 函数 lookup 在某处返回 0**，导致 BL 跳到了"错位"的 ROM 地址（不是函数入口而是函数中段的 `POP`）。这种 bug 在桌面 / Android 上也应可复现，**与 iOS 修改链无直接关系**。
  2. dispatcher trampoline 表初始化与 iOS 下时序错位，导致 stub 拿到的实现指针是 0。但 trampoline 是写到 ROM 里 patch 出来的（`dispatcher.cpp:202` memcpy），写入需要 PROT_WRITE —— 3.1 PROT_EXEC 剥除后 ROM chunk = RW，写入路径仍然合法。
  3. iOS sandbox 下某条 dispatch / SVC 处理误返回 0，再被 caller 当成函数指针调用。
- **结论（2026-05-23 跨系统对比落地）**：在 Apple Silicon Mac 上做了原生 arm64 桌面 Qt 编译，发现 macOS arm64 也复现了同样的现象（ZipManager 启动后立即 access violation writing 0x801000，进程 KERN-EXEC=3 被杀）。在 `common::commit` 里加 mprotect 失败诊断后，捕获到 `[commit-fail] ptr=0x14fabd000 size=0x1000 errno=22 (Invalid argument)` —— **真正的 root cause 是 Apple Silicon 用 16 KB host page，但 EKA2L1 memory model 用 4 KB 模拟页粒度调 mprotect，`len=0x1000` 不是 host page 整数倍被 macOS / iOS kernel 静默 EINVAL 拒绝**，对应页留在 PROT_NONE，下一次 guest 写就 SIGBUS / KERN_PROTECTION_FAILURE。3.2.1 的 PC=0 现场也是这条因果链的一种形态：caller `PUSH {r4..lr}` 把栈向下推 16 字节，所推页落在被 mprotect 拒绝的 host page 里，POP 时弹出来的是 PROT_NONE 触发的总线错误，dyncom 把后续的零字节继续往下解码就形成"PC=0 沿 page 扫描"的样子。
- **修法落地**：在 `src/emu/common/src/virtualmem.cpp` 的 `commit / change_protection` 里把 ptr 向下、size 向上对齐到 `sysconf(_SC_PAGESIZE)`；`decommit` 反过来只 PROT_NONE 完全覆盖的 host 页。改动只对 iOS / macOS arm64 生效，桌面 x86 / Linux / Android / Windows 行为不变。同步把 macOS arm64 也纳入 `translate_protection` 的 PROT_EXEC strip（同样 W^X 硬件）。提交 `779061f27 fix(ios+macos): align mprotect ranges to host page size`。
- macOS arm64 上验证 GUI app 已经能正常进入；iOS 端等 3.2 走完 Calculator + The Final Battle.sis 的双验收。
- 临时诊断日志已在后续清理中移除；3.2.2 的剩余问题单独记录在 [iOS Calculator EAGL 洋红空帧](./ios-calculator-eagl-magenta.md)。
- 验收（恢复跨系统对比之后再跑）：xcodebuildmcp 自动化点 Calculator → EmulatorView 渲染面在 10s 内出现非黑像素 → 单指 tap `1`、`+`、`1`、`=` → 截屏目视 `2`。归档 `docs/screenshots/ios-stage3/2-acceptance/`，stage 2 翻 ✅。
