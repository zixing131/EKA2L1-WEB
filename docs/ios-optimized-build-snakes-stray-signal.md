# iOS 优化构建下 Snakes guest panic 调查（已解决，2026-06-05）

## 结论

Snakes 在 iOS 真机优化构建中触发 `E32USER-CBase 46` 的根因位于同步 HLE sleep 的调度交接：

1. `thread::sleep()` 调用 `thread_scheduler::sleep(..., deque=true)`。
2. 调度器把当前线程设为 `thread_state::wait` 并移出 ready queue。
3. 该路径此前缺少 `kernel_system::prepare_reschedule()`，guest CPU 因此继续执行已经进入等待态的线程。
4. sleep timer 到期后，`thread::notify_sleep()` 调用 `dewait()`，将仍在执行的线程改为 `ready`。
5. 随后的 `WaitForAnyRequest` 在异常线程状态下跳过正常阻塞交接，guest 最终触发 active-scheduler stray-signal panic。

优化构建和真机运行时序扩大了这段窗口。通用修复是在同步 sleep 移出当前线程后立即请求 reschedule；`sleep_nof(..., deque=false)` 的异步语义保持原样。

## 关键证据

临时 `WFARDIAG` 探针在致命帧前记录到：

```text
DEWAIT 'Snakes' state=2
WAIT-BLOCK 'Snakes' count=-1 state=5
Thread Snakes panicked with category: E32USER-CBase and exit code: 46
```

- `state=2` 对应 `thread_state::wait`。
- `DEWAIT` 返回地址符号化到 `thread::notify_sleep()`。
- timer completion 与 audio DSP more-buffer completion 是有效请求完成源，它们负责暴露竞态窗口。
- Dyncom 在正常阻塞等待中已经观察到 stop budget；CPU stop 检查保持原实现。

## 代码修复

`src/emu/kernel/src/scheduler.cpp`：

- `thread_scheduler::sleep(..., deque=true)` 在当前线程进入等待态并移出 ready queue 后调用 `kern->prepare_reschedule()`。
- `reschedule()` 清理所属进程内存模型已经释放的 stale ready 线程，使 `switch_context()` 始终接收带有效地址空间的线程。
- 删除遗留的逐次上下文切换诊断日志。

修复采用通用调度器语义；app/game 专用分支保持原状。调查使用的 `WFARDIAG`、`SVCDIAG`、`SWIDIAG`、`VSYNCDIAG` 和候选 CPU-task-queue 代码均已清理。

## 真机验证

设备：iPhone Air，UDID `77611A2B-2A02-51FA-BAFC-2104F1D8011A`。

| 构建 | 应用 | 运行时间 | 结果 |
| --- | --- | ---: | --- |
| RelWithDebInfo | Snakes `0x2000730F` | 95 秒 | 进程存活；日志无 guest panic、DSA panic、SIGSEGV、access violation、emulation halt |
| RelWithDebInfo | Calculator `0x10005902` | 30 秒 | 进程存活；日志无 guest panic、SIGSEGV、access violation、emulation halt |
| Release `-O3` | Snakes `0x2000730F` | 95 秒 | 进程存活；214 行常规日志无 guest panic、DSA panic、SIGSEGV、access violation、emulation halt |

Release 构建与真机安装成功。Calculator/ZipManager 的 Release 启动命令进入前端启动流程，日志中未观察到目标 guest spawn；控制应用目标验证采用已通过的 RelWithDebInfo Calculator 结果。真机画面与声音由人工观察补充确认。

## 复现与验证命令

```sh
UDID=77611A2B-2A02-51FA-BAFC-2104F1D8011A

EKA2L1_IOS_DEVELOPMENT_TEAM=L6JP27B8YR \
EKA2L1_IOS_DEVICE=$UDID \
EKA2L1_IOS_CONFIGURATION=Release \
scripts/build_ios.sh install

xcrun devicectl device process launch --device $UDID --terminate-existing \
  com.eka2l1.emulator -LaunchAppUID 0x2000730F

xcrun devicectl device copy from --device $UDID --domain-type appDataContainer \
  --domain-identifier com.eka2l1.emulator \
  --source Documents/data/EKA2L1.log --destination /tmp/EKA2L1.log
```

日志重点检查：

```sh
rg "Thread Snakes panicked|DSA sync thread.*panicked|SIGSEGV|EXC_BAD_ACCESS|Access violation|Emulation halt" /tmp/EKA2L1.log
```

## 关联文档

- [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md)
- [`ios-device-missing-patch-dlls.md`](./ios-device-missing-patch-dlls.md)
