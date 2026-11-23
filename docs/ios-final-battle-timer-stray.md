# Final Battle 游戏内 `E32USER-CBase 46` stray-signal 崩溃

> **一句话结论**：周期定时器在 guest 写好 request status（pending）但还没执行
> `SetActive()` 的竞态窗口里就完成了请求，完成后该 active object 缺 `active`
> 标志、active scheduler 扫描视其为「没有就绪请求」→ stray-signal panic
> E32USER-CBase 46。让 timer 回调在「pending 但未 active」时短暂重排完成事件
> （有界）即可消除。

## 症状

The Final Battle（`uid=0xA0003C62`，N95/rm-320）进游戏后停留约 1 分钟自动弹出
guest fatal：`Process: FBattle / Exit type: panic / Category: E32USER-CBase /
Reason: 46`。仅 Release 构建易复现（Debug 时序不同不易触发）。

复现：打开 → 2s 进语言界面 → 虚拟键 `1` → 开始界面 `OK` → 游戏内文本再 `OK` →
正式游戏界面等约 1 分钟自动 fatal。

## 定位过程

1. panic 前 EKA2L1 会 dump guest active scheduler（`take_on_panic` 对 CBase
   41/42/43/46）。dump 显示所有 active object 都是 `Active, Pending`
   （`status=KRequestPending`），**没有任何就绪项** → 典型 stray signal：request
   semaphore 被多 signal 了一次，`CActiveScheduler::WaitForAnyRequest` 被唤醒却找
   不到可运行的 AO。
2. 在 `thread::signal_request` 用 `__builtin_return_address(0)` 记录最后一次
   signal 的 host 调用地址，再用 `atos`（按 `vmmap` 取 `__TEXT` 载入地址解 ASLR）
   解析 → 致命 signal 来自 **`eka2l1::kernel::timer_callback`**。
3. 在 `notify_info::complete` 记录 FBattle 线程每次完成的目标 status 的 pre-set
   `flags`：正常完成是 `flags=3`（`active|pending`），**致命那次是 `flags=2`
   （`pending` 但 `active` 未置位）**。

即：定时器完成了一个「已发起（pending）但 guest 还没 `SetActive()`」的请求。
完成后 `request_status::set` 清掉 pending、`active` 仍为 0；
`active_scheduler::has_ready_request` 要求 `status != pending && (flags & active)`
→ 漏判 → WaitForAnyRequest 拿到一个对不上任何就绪 AO 的信号 → panic 46。

这正是 `timer.cpp` 里既有 `MINIMUM_US_AFTER=30`（DDragon 注释：「signals an
object that has not yet been set to active in time」）想缓解的同一类竞态，30us
对 FBattle 不够。

## 为什么不能用 WaitForAnyRequest 「吸收 stray」来修

曾尝试扩展 `thread::wait_for_any_request` 既有的 stray-absorb hack（commit
`52623c00b`）去覆盖 fast-SVC 的 direct WaitForAnyRequest。结果**菜单按键卡死**：
该 hack 靠 `has_ready_request==false` 判定 stray，但菜单按键场景里这是一个
*尚未就绪* 的合法信号（guest 自己的 scheduler 能处理），吸收它等于吞掉按键。
absorb 方案无法区分「真 stray」与「AO 暂未就绪」，已放弃。

## 修复

`src/emu/kernel/src/timer.cpp` / `timer.h`：把 `timer_callback` 里的
`request_finish()` 换成新的 `timer::fire_or_defer()`。当回调触发时，如果目标
request status 是 `pending && !active`，且请求线程当前**未阻塞**在 request
semaphore（`request_count() >= 0`，即 guest 正在运行、处于 issue→SetActive 之间
的竞态），就把完成事件用 `schedule_event(TIMER_ACTIVATE_DEFER_US=100us)` 短暂
重排，让仍在运行的 guest 跑到 `SetActive()`；下次触发时 `active` 已置位即正常
完成。重排次数有界（`TIMER_ACTIVATE_DEFER_LIMIT=8`）。

- 对真机更忠实：真机上异步请求不可能在 `SetActive()` 之前完成。
- 零惩罚裸 `User::WaitForRequest`：这类裸 `TRequestStatus` 永不置 active，但等待
  时线程阻塞（`request_count() < 0`），不满足重排条件 → 立即完成、无额外延迟。
- EKA1 不带 active/pending 标志，`is_eka1()` 时按原逻辑立即完成。

## 验证（N95/rm-320，booted iPhone 16 Pro，Release）

- FBattle：语言→主菜单→进入真实游戏内（牢房场景、库存栏），游戏内方向/OK 输入
  正常，停留 100s+ 不再出现 E32USER-CBase 46。
- 控制 app Calculator（`0x10005902`）：完整 UI 正常渲染，无 panic，无回归。
- `./scripts/build_ios.sh simulator` 通过。

相关同类问题见 [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md)。
