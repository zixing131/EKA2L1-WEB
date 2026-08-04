# TestFlight 反复启动不同 app 偶现闪退：ipc_msg 引用计数破坏 → 线程对象 UAF / 堆污染

日期：2026-07-11 · Build：26.7.0 (260733) · 状态：已修复（模拟器验证，待 TestFlight 回归）

## 症状

同一台设备（iOS 26.5.2, iPhone18,4）同一 build 连续三份 .ips，共同场景是「反复打开不同 app」：

1. `06:11:53` — `EXC_BAD_ACCESS (0x100000030)`，`com.eka2l1.emulator.control` 队列：
   `runLaunchAppWithUID → bootDeviceAtIndex → ~system_impl → kernel_system::wipeout → ipc_msg::unref`。
   进程启动后 44s，即第二次启动 app 触发 reboot teardown 时崩。
2. `06:22:17` — `SIGBUS EXC_ARM_DA_ALIGN (0x9)`，主线程 `CFRelease`（CoreGraphics 填色状态）。存活 10 分钟。
3. `06:26:10` — `SIGSEGV (0x3)`，主线程 `objc_release`（UIStackView dealloc）。存活 41s。

崩溃 2/3 释放的是 0x3 / 0x9 这类小整数「指针」，且模拟器自身线程全部处于空闲等待——
是 malloc 堆先被本地代码踩坏后，UIKit 在主线程随机中枪的典型形态，与崩溃 1 同根因。

## 定位

CI 的 dSYM artifact（`EKA2L1-testflight-dSYM-<sha>`，UUID `5184E708`）符号化崩溃 1 的栈后，
再解码 .ips 里的 `instructionByteStream`：

```
ldr x8, [x0]        ; x8 = msg->own_thr        （ipc_msg 第一个成员）
cbz x8, ...
ldr x9, [x8]        ; x9 = own_thr 的 vtable    ← x9 = 0x100000000（已释放内存被复用）
ldr x9, [x9, #0x30] ; 取 decrease_access_count 虚表槽 ← 崩溃，far=0x100000030
blr x9
```

即 wipeout 强制析构残留 `ipc_msg` 时，`own_thr->decrease_access_count()` 的虚调用
命中一个**早已被释放的 kernel::thread**。

## 根因链

`kernel_system::free_msg` 原实现无条件强制释放：

```cpp
msg->type = ipc_message_type_wild;
msg->ref_count = 0;
```

`thread::do_cleanup()`（线程被杀时）对自己的 sync message 调 `free_msg`。若此时该消息
**仍挂在某个 server 上未被 complete**（ref_count=1），则：

1. 槽位立即变 `is_free()`，被 `create_msg` 回收给新线程 T2：`own_thr = T2`，`ref()` 给 T2 +1；
2. 老 server 迟到的 `message_complete`（同一 slot handle）`unref()` 1→0，**错误地给 T2 -1**，
   并把槽位再次标空；
3. T2 自己的 completion 再 `unref()`：`atomic<uint16>` 0→65535 **下溢**，槽位从此
   `ref_count=65535`、`own_thr=T2`，永久中毒；
4. T2 退出时 access_count 因步骤 2 被多减过，**提前归零 → 线程对象被释放**；
5. 下一次切换/重启设备：wipeout → `~ipc_msg` 见 `ref_count≠0` 强制 `unref` →
   对已释放 T2 虚调用 → 崩溃 1。
   在此之前，任何对悬垂 `own_thr` 的运行期访问（`signal_request` 写已释放的
   `request_sema`、`session_msg_link.deque()` 写已释放邻居）都在污染堆 → 崩溃 2/3。

配套的次级窗口：

- session 关闭时 `detatch` 会 unref 仍处 delivered 状态的消息，但 `delivered_msg_link`
  **不摘除**，server 之后 `receive()` 到已释放（可能已被回收）的槽位再 complete 一次；
- `message_ipc_copy` / `message_ipc_copy_eka1` 在 `ref()` 之后的 early-return 全部漏 `unref`；
- wipeout 中 sessions/servers 先于 msgs 释放，`~ipc_msg` 的强制 unref 会回调进这些已释放对象。

## 修复（均为内核通用修复，非 iOS hack）

- `kernel_system::free_msg`：仍被引用（ref_count>0）的消息不再强制清零，交给最终 unref 释放
  （属主线程被消息引用保活为僵尸，指针始终有效）；
- `ipc_msg::unref`：加下溢 guard（double-unref 记 WARN 后返回）；归零时同时摘除
  `delivered_msg_link`；sync 消息在属主线程已停止时回收槽位（type→wild、清 own_thr），
  避免固定 0x1000 槽池泄漏；
- `kernel_system::wipeout`：析构 msgs 前先清 `own_thr`/`msg_session`/`ref_count`，
  使 teardown 不触碰早已释放的 session/server/thread；
- `session::destroy`：清 pool 消息的 `msg_session` 防回调已释放 session；
  `disconnect_msg_->own_thr` 判空；
- `svc.cpp`：补齐 `message_ipc_copy(_eka1)` 各 early-return 的 `unref`。

## 验证

- Release 模拟器回归 8/8 PASS（Final Battle + Calculator，本身即覆盖二次启动 reboot 路径）；
- 额外连续 5 次交替启动 Final Battle ↔ Calculator（每次触发完整 teardown+重建），
  进程存活、日志无 `already-released` WARN、无新 crash report。
- TestFlight 用户侧「反复开不同 app 偶现闪退」待下一 build 回归确认。
