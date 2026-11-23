# N95 Snakes 启动画面卡死 / E32USER-CBase 46 误信号

> 来源：阶段 3 修复清单 #6（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决。
>
> **一句话结论**：Snakes (`0x2000730F`) 卡住是请求信号被误消耗和补发导致的调度失衡，修正请求等待与通知完成语义后，游戏可进入主菜单并能实际游玩。

## 现象

- N95 / S60v3 上启动 Snakes (`uid=0x2000730F`) 后，游戏能显示 `snakes` splash / Nokia intro，但随后冻结，无法进入主菜单。
- 日志中 guest active scheduler 最终以 `E32USER-CBase 46` panic。语义上这是 stray signal：`CActiveScheduler::WaitForAnyRequest` 被额外 request signal 唤醒，但没有任何 active object 的 `TRequestStatus` 处于 ready 状态。
- panic 后 Snakes 线程死亡，graphics command list 留在半构建状态，后续出现 `Corrupted graphics command list! Emulation halt` 一类二次错误。
- 同一问题也能在 macOS desktop 路径上复现，说明主因在共享 kernel/services 语义，不是 iOS 前端或 CPU backend 单点问题。

## 排查事实链

### 1. HLE sleep 泄漏 request signal

Window server 的 frame pacing 会在动画帧里通过 `canvas_base::try_update` 调到 client thread 的 `drawer->sleep()`。旧的 `thread::sleep()` 行为是：

1. 在当前 guest thread 上循环 `wait_for_any_request()`，可能阻塞并消费 guest request semaphore。
2. 同时 arm 一个 host-side wakeup timer。
3. 如果真实 IPC/notify completion 先唤醒线程，迟到的 sleep timer 仍会在 `notify_sleep` 里按 `sleep_level` 补发 request signal。

这会把 host-side frame pacing sleep 混进 guest request semaphore 语义中。Snakes splash 阶段动画密集，泄漏会累积成 active scheduler 后续看到的 stray signal。

### 2. 残余卡死来自 ready active object 与 semaphore count 失配

清掉 HLE sleep 泄漏后，Snakes 不再快速 panic，但仍可卡在 splash。轻量诊断显示典型循环为：

- Snakes 在 `WaitForAnyRequest` stub 附近等待，request semaphore count 可到 `0` 或负值。
- 同时 active scheduler 队列里已经存在 `status != pending && active` 的 ready active object。
- 旧 `wait_for_any_request()` 只看 request semaphore，不看 active scheduler ready 状态；于是已经 ready 的 active object 没机会回到 guest 侧 dispatch，线程继续阻塞。

这说明 EKA2L1 当前共享 request semaphore 模型需要 hardening：在 direct `User::WaitForAnyRequest` 上，semaphore count 与 guest `TRequestStatus` ready 状态可能因旧泄漏或异步完成时序短暂失配；此时应以 guest active scheduler 队列的 ready active object 为准返回给 guest 调度。

### 3. 需要区分 WaitForAnyRequest 与 WaitForRequest wrapper

EKA2L1 里同步 IPC completion 与 active-object completion 共用同一个 thread request semaphore。`User::WaitForAnyRequest()` 用于 active scheduler；`User::WaitForRequest(TRequestStatus&)` 用于等待明确的 request status。

如果在 `wait_for_any_request()` 中盲目吸收 stale signals，会误吞 `WaitForRequest(TRequestStatus&)` wrapper 或其它直接 wait 的信号，破坏同步 IPC / 非 active-object request 的语义。因此最终实现通过当前 PC 附近的 guest stub 指令识别：

- direct `User::WaitForAnyRequest` stub 才允许吸收 stale signal；
- fast SVC 形态若 `R0` 指向有效 `TRequestStatus`，视作 `WaitForRequest(TRequestStatus&)` wrapper，不能按 active scheduler stale signal 处理；
- arbitrary waits / non-active request statuses 不做吞信号处理。

### 4. vsync notify completion 需要 kernel lock

`screen_post_transferer::complete_notify()` 原本可在 timing thread 回调中直接调用 `notify_info::complete()`，写 guest `TRequestStatus` 并 signal requester。这个路径没有持有 kernel lock，和其它 kernel completion 路径的习惯不一致，也会扩大 request status / semaphore 时序窗口。

修法把 pending vsync notify 先在 `screen_post_transferer` 自己的 mutex 下从 `vsync_notifies_` 移除，然后取 requester 的 `kernel_system`，在 kernel lock 下完成 `TRequestStatus` + signal，最后释放 notify 对象。

## 修法

### `thread::sleep()`

- 把 HLE frame pacing sleep 改成纯 scheduler sleep：`scheduler->sleep(this, ussecs, true)`。
- 不再循环 `wait_for_any_request()` 消费 guest request semaphore。
- 普通 sleep timer 到期时只 `scheduler->dewait(this)`，不再补发 guest request signal。
- `sleep_nof(TRequestStatus)` 路径保持通过 request status completion + `signal_request()` 唤醒。

### `thread::wait_for_any_request()`

- 新增本地 helper 识别 direct `User::WaitForAnyRequest` stub 和 `WaitForRequest(TRequestStatus&)` wrapper。
- 如果 semaphore count 已经非正，但 active scheduler 已有 ready active object，且当前确实处于 direct `WaitForAnyRequest` stub，则直接返回给 guest dispatch ready AO。
- 普通等待被唤醒后，如果 active scheduler 不存在、已有 ready request、或当前不是 direct `WaitForAnyRequest` stub，就停止吞信号。
- 只有 direct `WaitForAnyRequest` stub 且 active scheduler 没有 ready AO 时，才继续吸收 stale semaphore signals。

### `active_scheduler`

- 抽出 `active_scheduler::has_ready_request()`，复用 `status != epoc::status_pending && active` 的 ready 判定。
- `check_stray()` 先调用该 helper；如果还有 ready active object，不报告 stray。

### `screen_post_transferer::complete_notify()`

- timing callback 触发后，先在 `screen_post_transferer` mutex 下确认 notify 仍 pending 并从列表移除。
- 在 requester 所属 kernel lock 下调用 `notify_info::complete(epoc::error_none)`。
- 如果 notify 已被 cancel/destroy 路径移除，只释放 copy 并返回。

## 设计判断

这不是为了 Snakes 单点跑通加的 UID/名称补丁：

- 改动不包含 Snakes UID / 进程名分支。
- `thread::sleep()` 的修复针对 HLE frame pacing 与 guest request semaphore 的边界，是通用语义修正。
- `WaitForAnyRequest` 的 hardening 仍沿用 EKA2L1 现有“thread request semaphore + guest `TRequestStatus`”模型，没有临时引入 per-app completion queue。
- 识别 direct wait stub 的目的是保护同步 `WaitForRequest(TRequestStatus&)` 和 arbitrary waits，不扩大吞信号范围。
- vsync notify completion 改为 kernel lock 下完成，符合其它 kernel request completion 的线程安全习惯。

需要注意的是，这仍不是一次完整的 per-request completion 模型重写；它是在当前共享 semaphore 模型下，让 direct active-scheduler wait、sync wait wrapper 与 HLE pacing sleep 三者之间保持平衡。

## 验证

环境：iPhone 16 Pro iOS 26.5 simulator，N95 device index 0，bundle id `com.eka2l1.emulator`。

- 清理所有 `REQDBG` / 临时诊断代码后，`xcodebuildmcp simulator build` 通过。
- `-LaunchAppUID 0x2000730F` 连续多次启动 Snakes，均能从 splash 进入主菜单。
- 使用软键盘确认 `Start Game` → `Start New Game` → 跳过玩法提示后进入实际 3D 游戏关卡；方向键输入后画面继续实时更新。
- Calculator (`uid=0x10005902`) 回归正常渲染。
- 日志扫描无 `REQDBG`、`Thread Snakes panicked`、`E32USER-CBase`、`Access violation`、`Corrupted graphics`、`Active scheduler dump`、`KERN-EXEC`、`Emulation halt`。

## 真机（device）回归（2026-06-03）—— ⚠️ 旧结论已被推翻，真机根因另在他处

> 本节最初把真机黑屏归因为「同一非确定性 request-completion / active-scheduler 竞态」。**该结论已被证伪**：那个 `wait_for_any_request` 的 block/complete 高频循环其实是**游戏正常的逐帧循环**，不是活锁；加过的 kernel WFARDBG 诊断已全部回退。
>
> 真机 N95 黑屏无声的**真实根因是 iOS 真机构建从不安装 HLE patch DLL**（缺音频 patch → 无声；缺 `scdv` 屏幕驱动 patch → Snakes 退回 `d_display.ldd` 直屏路径触发活动调度器 panic → 黑屏），与本文的模拟器 stray-signal 修复无关。已修复并真机验证（声音+画面正常）。完整调查见 **[`ios-device-missing-patch-dlls.md`](./ios-device-missing-patch-dlls.md)**。本文档（模拟器 N95 上的 stray-signal 修复）本身仍然成立。
