# Stray-signal 根因与根治方向（请求信号量记账重构，接手指南）

> 状态：🟢 **根因已找到并修复（2026-08-03）**，见
> [`dsa-cancel-double-completion-stray.md`](./dsa-cancel-double-completion-stray.md)。
> **吸收机制已完全下线**：`thread::wait_for_any_request()` 不再吞信号，配套的
> `stray_absorbed_refund_` 补偿预算一并删除；也没有留计数器/诊断日志——判别 direct
> `WaitForAnyRequest` 与 `WaitForRequest` wrapper 靠的是共享 exec stub 上的 r0，据此统计
> 出来的 "stray" 在健康 title 上照样会报（N-Gage Tetris 一局 ~10 次），留着只会误导排查。
> Snakes / FBattle / Calculator / Angry Birds / N-Gage Tetris 实测均正常。
>
> **一句话结论**：多出来的那一个信号是 **DSA 取消被完成了两次**——`RDirectScreenAccess::
> Cancel()` 是客户端自完成的请求（ws32 的 `CDirectScreenAccess::DoCancel()` 紧接着调
> `User::RequestComplete(iStatus, KErrCancel)`，再由 `CActive::Cancel()` 的
> `User::WaitForRequest` 吃掉那一发），而 EKA2L1 的 `dsa::do_cancel()` 又在服务端完成了
> 一次。修复 = 客户端发起的 cancel 不在服务端完成，只丢弃请求。
>
> 本文以下内容保留作为**调查方法与死路记录**（聚合不变量检测器、ROM 导出表符号化、
> 计数器层面 A/B 的天花板等），对后续同类问题仍然有用。

## 1. 问题模型

真机 Symbian 语义：guest 线程的 request semaphore 上，每个 `RequestComplete`
恰好对应一次信号；`CActiveScheduler::Run` 每从 `WaitForAnyRequest` 醒来一次，
就必然能在 AO 队列里找到一个 `active && status != KRequestPending` 的对象。
找不到 = 程序错误 = panic `E32USER-CBase 46`。真机**没有吸收机制**。

EKA2L1 里同一个 semaphore 同时承载：AO 完成、同步 IPC（`SendReceive` sync /
`User::WaitForRequest` wrapper）完成、以及历史上混进来过的 HLE 侧唤醒。任何一处
HLE 代码多发 / 早发 / 补发信号，账就不平，guest 调度器醒来找不到就绪 AO → panic。

## 2. 现有防线（按时间）

| 防线 | 位置 | 修掉的来源 | 文档 |
|------|------|-----------|------|
| `thread::sleep` 双唤醒泄漏修复 | `kernel/src/thread.cpp` | HLE frame-pacing sleep 混入 request semaphore | [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md) |
| vsync notify 走 kernel lock | `dispatch/src/screen.cpp` | 无锁完成扩大时序窗口 | 同上 |
| timer `fire_or_defer` | `kernel/src/timer.cpp` | 定时器在 guest 写好 status 但未 `SetActive` 的窗口内完成 | [`ios-final-battle-timer-stray.md`](./ios-final-battle-timer-stray.md) |
| **吸收机制**（本文主角） | `thread::wait_for_any_request` | 兜住所有残留 stray | [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md) + 2026-07-07 变更日志 |
| 吸收机制的 dynarmic 修复 | 同上 | ctx 快照陈旧 + fast-stub 被拒 → JIT 下吸收失效 | 变更日志 2026-07-07 |

吸收机制的当前形态（三重保护，缺一不可）：

- 只在 `identify_wait_request_stub` 识别为 **direct `User::WaitForAnyRequest`**
  （slow/patched/fast exec stub + `BX LR` 返回）时吸收；
- fast-exec 形态若 **r0 映射到有效内存**，视为 `User::WaitForRequest(TRequestStatus&)`
  wrapper，**绝不吸收**（wrapper 的 do-while 循环会自然吃掉 stray；盲吸收会吞掉
  wrapper 的真信号 → 历史上的"菜单按键被吞死锁"）；
- active scheduler 存在且 `has_ready_request()` 为假才吸收（有就绪 AO 时立即交还 guest）。

## 3. 量化证据（2026-07-07 探针实测，iPhone 16 Pro sim）

- **dyncom 下 Snakes 一次启动画面 ≈ 235 个 stray 被吸收**——机制不是死代码，去掉立即回归 panic。
- dynarmic 下吸收失效（当时的 bug）= Snakes 启动必现 panic，等价于一次"自然去除实验"。
- stray 到达位置的分布随后端时序漂移：dyncom（慢）下多在 park 后到达；dynarmic（快）下
  在 guest 到达 WaitForAnyRequest 前已入队。**dynarmic 是这个问题的时序放大器，
  复现/验证请优先用 JIT 后端压测。**

## 4. 残留 stray 的来源（未根治部分）

探针（见 §6 配方）已排除的嫌疑：

- **双重完成**：未观测到（`notify_info::complete` 完成后清 `sts`，拷贝副本二次完成为 no-op）。
- **complete-before-SetActive 本身**：`fifo::set_listener` / `msv listen` 等在
  SendReceive 内立即完成是合法 Symbian 语义（guest 随后 `SetActive`，信号与就绪 AO
  配对），不是 stray。

先前调查（2026-06）对残留信号做过 reason-tagging，当时的结论是"残留以
`hle-ipc-complete` 为主"。**该结论已在 2026-08-03 被证伪，见 §4bis。**

其他已知可疑点（未逐一审计）：

- `thread.cpp` 异常处理路径：`call_exception_handler` 中 `backup_state == wait_fast_sema`
  时补发 `signal_request()`，与 `restore_before_exception_state` 的
  `wait_for_any_request()` 是否严格配对；
- `semaphore::timeouted` / wait 超时路径与 request semaphore 的交互。

## 4bis. 2026-08-03 账本复查：把范围收敛到"一个多余的显式信号"

复现环境：N95（rm-320）+ Snakes `0x2000730F`，Release 模拟器构建，dyncom 与 dynarmic
均可。诊断手段是一套临时"信号账本"：`signal_request` 每次入队一个 token（seq、目标
request status 地址、`set()` 之前的 flags、完成码、`__builtin_return_address(0..2)` 经
`dladdr` 符号化、服务名与 opcode），`wait_for_any_request` 每个出口消费一个 token 并
记录 pot / 就绪 AO 地址；panic 时整本 dump。

### 结论（按可信度排序）

1. **吸收机制确实还在承重。** 把吸收分支改成 `break`（env 开关），Snakes 在 N95 上
   4/4 复现 `E32USER-CBase 46`。所以"absorbed=0"不等于"不需要它"——见下条。
2. **`Absorbed stray` 这行日志在 iOS 默认配置下根本打不出来。** `BUILD_FOR_USER` 的
   `LOG_FILTER_NORMAL_USE_PRESET` 含 `Kernel:Warn`，`LOG_INFO(KERNEL, ...)` 全部被吞。
   任何靠 grep `Absorbed stray` 得出的"计数为 0"结论都是无效的；诊断请一律用
   `LOG_ERROR` 或打开 `extensive-logging`。
3. **总量是平的。** 一次完整启动 `signals == waits`（例如 1308/1308）。**不是多发信号
   的问题。**
4. **源头侧无过量完成。** 在 `signal_request` 里检查"被完成的 status 在 `set()` 之前
   是否带 `pending`"，整个启动 0 命中。即：不存在二次完成、不存在完成一个未发起的
   请求。`session::detatch` 重复完成（HLE 完成后既不置 `msg_status=completed` 也不摘
   `in_progress_msgs_` 链）这条看起来很像的路径，实测同样 0 命中。
5. **不平的量是 1，且在 guest 的 active scheduler 第一次跑起来之前就已经存在。**
   在"direct `User::WaitForAnyRequest` + 已安装 active scheduler"这个循环头上，真机
   不变量是 `request_sema->count() == 就绪 AO 个数`。实测第一次进入调度器循环头时
   `pot=11 / ready=10`，`shortcut=0 / absorbed=0 / refund=0`，此后这个 +1 一直漂着，
   直到某次 AO 流枯竭才引爆 panic。
6. **多出来的那一个是显式信号，不是完成信号。** 把窗口内所有 token 按 status 地址配
   对后，唯一"有信号但对不上任何 request status"的来源是 `RThread::RequestSignal()`
   （`Exec::ThreadRequestSignal`，v93 = SVC `0xDC`，handle 恒为 `KCurrentThreadHandle`
   `0xFFFF8001`，自己signal自己）。启动阶段 Snakes 线程调用 3 次，其余带 status 的未
   消费 token（6 个）在 panic dump 的 AO 队列里都能找到对应的就绪 AO。

也就是说：**账不平的是"guest 少做了一次 wait"，而不是"EKA2L1 多发了一次 signal"。**

### 因果确认：`Exec::ThreadRequestSignal` 就是那个 stray（A/B 实测）

两个 env 开关，N95 + Snakes，每组各跑 3 次：

| 条件 | 结果 |
|------|------|
| `EKA2L1_NOABSORB=1`（只关吸收） | **3/3 `E32USER-CBase 46`** |
| `EKA2L1_NOABSORB=1` + `EKA2L1_NOTRS=1`（同时把 `thread_request_signal` 变 no-op） | **3/3 无 panic**，正常进主菜单 42 FPS |

即：**把这一条 signal 路径去掉，吸收机制就完全不需要了。** 这是目前为止最强的因果证据。

补充观测（决定下一步方向的关键限定）：

- guest 每帧都在调 `RThread::RequestSignal()`（调用链 `6r45_1b.exe` → `CONE.DLL` →
  euser），一次运行几百次；**但 surplus 全程只涨到 1 就不再涨**（检测器只在变大时打印，
  整个运行只出现一行）。所以**绝大多数 RequestSignal 是配平的，只有启动阶段那 3 次里的
  某一次没被消费**。不要把"RequestSignal 一律有害"当结论。
- SVC 号映射是对的，不要怀疑：v93 的 `0xDC` 调用时 `r0 == 0xFFFF8001`
  （`KCurrentThreadHandle`，即 `RThread()` 的默认 handle），符合
  `RThread::RequestSignal()` 的签名；相邻的 `EExecExceptionDescriptor` 收的是代码地址，
  形状对不上。
- 关掉它 guest 不会挂死，说明这些自 signal 是"催一下调度器"性质的、可丢的唤醒；真机上
  它们由 `CActiveScheduler::Run` 循环头消费掉。**所以下一步要查的是：为什么 EKA2L1 里
  启动阶段有一次这样的唤醒没有对应的 wait**（最可能是 HLE 把某个本该异步的东西同步完成
  了，导致 guest 的嵌套 `CActiveScheduler::Run` 循环提前退出，`AsyncStop` 的那一发就没人
  接）。

### 再往下走的两条实测（2026-08-03 同日）

**① 计数器层面已经问不出"是哪一发"。** 加了 `EKA2L1_TRS_SKIP=<n>`（跳过第 n 次
`thread_request_signal`）后，跳过 #3 / #4 / #5（启动阶段 Snakes 线程的三次）**三者效果
完全一样**：surplus 都归零、都不 panic、都能 40 FPS 进主菜单。因为 semaphore 是纯计数
器，少发任意一发都等价，而 guest 对少一次"催调度器"的唤醒又足够宽容。**别再设计这类
A/B 去定位"哪一发"，它在原理上就区分不了。**

**② 唯一带方向性的线索：TRS #4 后面没有 wait。** 在 `lib_manager::call_svc` 里记录每次
TRS 之后该线程的 10 条 SVC：

| TRS | 线程 | 随后 10 条 SVC 里的 `0x800000`(WaitForAnyRequest) |
|-----|------|---|
| #3 | Snakes | 第 1、2 条就是 |
| **#4** | **Snakes** | **一条都没有**（`0x4D`，然后 5× `0x800008`…） |
| #5 | Snakes | 第 9 条 |
| #6 | Snakes | 第 2 条 |

TRS #4 的调用链也和别的不同——是一条很深的纯 CONE 链（游戏 exe `0x70000718` → CONE
`0x8078368E → 0x80783674 → 0x807835D4 → 0x8078357A → 0x807834C0 → 0x80784CAC →
0x80786820 → 0x8078A17C` → euser），形状像 `CCoeEnv` 构造/启动路径，而不是 #3 那种
`WS32.DLL` 直接调用。**下一个接手的人应该从 TRS #4 这条 CONE 链入手**，把这几个 CONE
地址对到具体函数（需要 N95 ROM 里 `CONE.DLL` 的导出表 + 对应版本的 ordinal 表）。

### 取 guest 调用栈的注意事项

- `kern->get_cpu()->get_lr()` / `get_reg(14)` / `save_context()` 在**慢 SVC**（如 `0xDC`）
  上读到 0，拿不到返回地址；同样的手段在**快 SVC**（`wait_for_any_request`）上却是准的。
  想定位 guest 调用者只能扫栈。
- 扫栈配方：从 `get_cpu()->get_sp()` 往上取若干字，凡是 `>= 0x50000000` 的就用
  `svc.cpp` 里现成的 `get_dll_full_path()`（内部先试 `get_codeseg_from_addr`，再回退到
  ROM 目录按地址区间反查 XIP 镜像）解析成 DLL 名。这样能直接读出
  `euser / CONE.DLL / eikcore.dll / APPARC.DLL / 游戏 exe` 这样的调用链。
- **栈上第一个像代码地址的字不一定是真正的返回地址**（可能是上一次调用留下的），据此
  推断"直接调用者"会得到错误结论（本次就先误判成 WS32.DLL 的
  `EventReadyCancel`，加日志后发现该路径压根没被走到）。
- 想把噪声压下去，就**校验候选返回地址的前一条指令是不是 BL/BLX**，只保留通过校验的：
  Thumb 32 位 BL/BLX = `(hw[ret-4] & 0xF800) == 0xF000 && (hw[ret-2] & 0xF800) ∈
  {0xF800, 0xE800}`（配合 `ret & 1` 判 Thumb），16 位 `BLX Rm` = `(hw[ret-2] & 0xFF80)
  == 0x4780`，ARM = `(w[ret-4] & 0x0F000000) == 0x0B000000` 或 `BLX imm`
  (`0xFA000000` 掩码)。这样打出来的链条可读性大幅提高。

### 顺带确认的一个真 bug：stub 识别靠 r0 是不可靠的

`identify_wait_request_stub` 用"r0 能否映射成有效 guest 内存"来区分
`User::WaitForAnyRequest()` 和 `User::WaitForRequest(TRequestStatus&)`。实测：

- 两者走的是**同一个** fast-exec stub（本 ROM 上 `pc=0x803A034C`），**且返回地址
  `lr` 也完全相同**（`0x803A8B95`，400 次采样无一例外）——所以 pc/lr 都不能用来区分。
- wrapper 的 do-while 第二圈起，r0 里放的是刚 load 出来的 status **值**，常常正好是
  `KRequestPending = 0x80000001`。这个值落在 ROM 里、`ptr::get()` 返回非空，于是被
  当成"合法 TRequestStatus 指针"→ 判成 wrapper。一次启动里有 12 次这样的等待。

反过来，如果 wrapper 循环里 r0 恰好是 0 或别的映射不到的值，就会被判成 direct，吸收
机制就可能吃掉 wrapper 的真信号（这正是 N-Gage Tetris 那次卡死的形状）。**这套判别
本质上是掷骰子。**

⚠️ **不要试图"收紧 r0 判据"**：把条件改成"r0 必须落在可写用户内存（`< 0x80000000`）"
之后，上面那 12 次 wrapper 迭代全部被判成 direct，`(pot<=0 && has_ready_request)` 的
shortcut 分支于是不消费就返回，guest 立刻活锁——实测 316 万次 `shortcut-ready`、
黑屏 0 FPS。已回退。

## 5. 根治方向（按 2026-08-03 结果修订）

目标不变：**让"调度器循环头 pot == 就绪 AO 数"这个不变量恒成立**，然后把吸收降级为
断言/诊断日志，最终删除。§4bis 之后建议的顺序：

1. **先查启动阶段那 1 次没被消费的 `RThread::RequestSignal()`**（这是当前唯一未闭合的
   账目，因果已由 §4bis 的 A/B 坐实）。已知调用链是 游戏 exe → `CONE.DLL` → euser；
   需要确认它对应的是 `CActiveScheduler::Stop()` / `CActiveSchedulerWait::AsyncStop()`
   的哪一种形态，以及为什么对应的嵌套 `CActiveScheduler::Run` 循环头没有把它吃掉。
   怀疑方向：某个 HLE 服务把本该异步的请求在 `SendReceive` 内同步完成了，导致嵌套调度
   循环比真机早退出一轮。**具体入口 = TRS #4 那条纯 CONE 调用链**（见 §4bis）。取栈方法
   见 §4bis 末尾；注意计数器层面的 A/B 已经证明问不出"是哪一发"。
2. **不要再花时间做 per-signal 身份配对**。guest 的 `User::WaitForRequest` 会借走别人
   的信号再用 `RThread::RequestSignal(n)`（v93 SVC `0x3B`）还回去，一次启动就有 20~29
   次借还，**信号身份在 semaphore 这一层被彻底抹掉**，离线配对必然对不齐。可用的只有
   聚合不变量（§4bis 第 5 条）。
3. **可观测性修正**：吸收计数那行必须是 `LOG_ERROR`（`Kernel:Warn` 会吞掉 `LOG_INFO`），
   否则度量结果是假的。
4. **候选设计**（按侵入性排序）：
   - a. 修单点：定位到具体失衡调用点后按 `fire_or_defer` 的思路逐个修（侵入最小）；
   - b. 把吸收升级成**对账**：在调度器循环头按 `pot - ready` 一次性补/削到相等。比现在
     "一次吃一个、靠 `stray_absorbed_refund_` 找补"更有依据，但仍然是补偿层，且会吃掉
     `AsyncStop` 这类合法的"无就绪 AO 唤醒"，落地前必须先解决第 1 条；
   - c. 分离计数：同步 IPC 完成不再走共享 request semaphore，改专用事件/计数
     （更接近真机 `iRequestSemaphore` 只服务 AO 的用法；侵入 kernel/ipc 层，
     需过全平台回归）；
   - d. 对照 upstream：diff 本 fork 与 upstream EKA2L1 的 `wait_for_any_request` /
     completion 语义，确认哪些失衡是 fork 引入、哪些 upstream 同样存在。
5. **验收标准**：Snakes（splash + 3D gameplay）、Final Battle、Calculator（Options
   菜单开关）、BIA 全流程在 **dyncom 与 dynarmic 双后端**下 `absorbed=0` 且回归
   8/8、BIA 7/7；然后把吸收换成 `LOG_ERROR` + 计数，跑一个版本周期无报告后删除。

## 6. 探针配方（本次调试验证有效，均为临时代码，勿入提交）

2026-08-03 这轮用的一套（比 2026-06 的 reason-tagging 好用，推荐直接复用）：

- **信号账本**：`thread` 上挂 `std::deque<token>`；`signal_request` 每次 push 一个
  token（seq / 目标 status 地址 / `set()` 前的 flags / 完成码 / 服务名 + opcode /
  `__builtin_return_address(0..2)`），`wait_for_any_request` 每个出口 pop 一个并记录
  出口类型、pot、`r0`、首个就绪 AO 地址。**注意 park（`state != run`）那条出口是"欠账"
  而不是"消费"**：token 要等下一次 signal 补记，否则账本会错位。
- **符号化**：直接在 dump 里用 `dladdr()` 出符号名，比事后 `atos` + ASLR 省事得多。
- **源头过量检测**：`signal_request` 里检查目标 status 在 `set()` 之前是否带 `pending`；
  不带就是凭空多出来的信号。这条能一次性证伪"双重完成 / detatch 重复完成"整类猜想。
- **聚合不变量检测**：在 `wait_for_any_request` 循环头，当
  `stub_pre.direct_wait_for_any_request && act_sched_pre` 时比较 `before_count` 与
  `active_scheduler::ready_request_count()`；差值变大就 dump 上一段窗口内的全部 token。
  **这是唯一不依赖 panic、可以连续监测的检测器**，强烈建议先建它再动手改代码。
- **A/B 开关走 env**：`SIMCTL_CHILD_<VAR>` 透传给模拟器进程，改一行 `getenv` 就能做
  "关掉吸收 / 关掉某条 signal 路径"的对照实验，不用重新编译。
- **探针本身会改时序**：这个 stray 是竞态，加一次 `fmt::format` 就可能从"必现"变成
  "跑 5 次不现"。诊断代码要尽量只存 POD、把格式化推到 dump 时再做。

- **完成源标记**：`notify_info::complete` 里按 `sts_real->flags`（pending/active）+
  `requester->request_count()` 过滤，打 `__builtin_return_address(0)`；
  用 `sample <pid> 1` 拿 EKA2L1 加载地址，`atos -o EKA2L1.app/EKA2L1 -l <load>` 解析。
- **吸收路径插桩**：`wait_for_any_request` 的 stub-miss / absorb 分支打
  `ctx.get_pc()`、`*(pc-4)`（SVC 编码）、返回指令、`r0`。
- **SVC 入口 PC 校验**：`kernel_system::set_epoc_version` 的 `system_call_handler`
  里校验 `*(live_pc - 4)` 是否 SVC 编码（`(ins & 0x0F000000) == 0x0F000000`），
  验证后端上下文报告准确性（dyncom/dynarmic 均已验证准确）。
- 日志位置：`Documents/data/EKA2L1.log`；**活配置是 `Documents/data/config.yml`**
  （`Documents/config.yml` 是陈旧遗留，别读错）。

## 7. 风险与红线

- **绝不能盲吸收**：没有 stub 识别 + wrapper 判别 + ready-AO 检查的任何"吞信号"
  扩展都会复现"按键被吞 / 同步 IPC 死锁"（历史已踩过两次）。
- 改动 `wait_for_any_request` / completion 语义后，dyncom 与 dynarmic **两个后端都要**
  过回归（时序不同，单后端通过不代表安全），最少：`scripts/ios_regression_test.sh` 8/8 ×2 +
  Snakes 进 3D gameplay ×2 + `scripts/ios_bia_gameplay_test.sh` 7/7。
- EKA1 不携带 request_status flags，所有基于 flags 的判断都要保持 `is_eka1()` 分支语义。
