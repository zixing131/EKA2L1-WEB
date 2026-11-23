# iOS TestFlight 崩溃三分类与修复 · 第二轮（26.7.0 / build 260713）

第一轮见 [[ios-testflight-crash-triage]]（A 类 `create_bitmap` 分配器竞争与 B1–B3 已上线）。
本轮针对 `crashes/` 下新导出的 5 个崩溃点（`BfrLJ_TCel9uJnRKSu5mdE`、`BZS3HhjnQgmS5EP5KfkwiM`、
`CkaoCnpe2ABfCUUMGe8Daj`、`CziXWKgw_CPxhoVVNwl7sF`、`DVNiQ9Mf2AhRA95jbeZcO6`，共 24 份日志）逐份归因。

结论：Xcode 的聚类把不同根因混在同一崩溃点里，逐份拆栈后收敛为 **6 个独立根因，全部已修复**。
两大主线：①（占多数）**退出游戏 → 下次启动触发 `bootDeviceAtIndex:` 整机重建**时，旧 `eka2l1::system`
的销毁与仍在运行的线程（ntimer 定时器线程、applist 图标扫描线程池）互相踩踏，以及 `kernel_system::wipeout()`
自身的析构顺序缺陷；②（运行期）codedump 收集器悬垂节点、applist 父子进程悬垂指针、sensor 空驱动、
位图哈希越界读。第一轮遗留的 B5（哈希越界）与 B6（FreeType 堆损坏）在本轮均有解释与修复。

---

## 崩溃点 → 根因对照

| 崩溃点 | 日志形态 | 根因编号 |
|--------|----------|----------|
| `CkaoCnpe2ABfCUUMGe8Daj` / `BfrLJ` / `CziX` 部分 | `bootDeviceAtIndex:` → `~system` → `wipeout` → `process::kill` → `~flexible_mem_model_process` → `detach_mapping` SIGSEGV @0x20 | R2 |
| `BfrLJ`（NO_CRASH_STACK）另 3 份 | applist 线程池 worker 在 `create_bitmap` 锁一个**已释放**的 `recursive_mutex`（@0x1e8）；同时另一线程在 `bootDeviceAtIndex:` 重建系统 | R6 |
| `DVNiQ9Mf2AhRA95jbeZcO6`（退出游戏时，非必现） | ① `thread_kill` → `process::kill` → `codeseg::detach` → `free_attached_data` ↔ `codedump_collector::add` 递归 SIGSEGV；② ntimer 线程 `kernel::timer_callback` SIGSEGV；③ ntimer 线程动画重绘 `FT_Load_Glyph` malloc abort（= 第一轮 B6）；④ rescan worker `create_bitmap` SIGSEGV | ①R3 ②③R1 ④R6 |
| `BZS3HhjnQgmS5EP5KfkwiM` | ① `hash_bitwise_bitmap` → `XXH64_update` memmove SIGBUS/SIGSEGV（页对齐，= 第一轮 B5）；② `finish_logons` → applist launch_app logon lambda SIGSEGV；③ `sensor query_channels` SIGSEGV @0x0；④ `kernel_obj::full_name` owner 链 SIGBUS（疑 R4 下游） | ①R5 ②R4 ③（sensor）④R4 |
| `CziXWKgw_CPxhoVVNwl7sF` | ① 同 R2 的 teardown 栈；② 同 sensor 空驱动栈 | R2 / sensor |

---

## 已修复的根因

### R1. ntimer 定时器线程在系统析构期仍在跑回调（跨平台）

`~system_impl` 先 `kern_->wipeout()`、最后才 `timing_.reset()`。期间 ntimer 线程照常触发事件：
`kernel::timer_callback` 摸到正被销毁的 kernel timer（SIGSEGV），动画调度器整屏重绘走
`screen::redraw → font_atlas → FT_Load_Glyph` 摸到正被释放的字体/窗口状态（malloc abort，即第一轮 B6 的
「宿主堆损坏」现场）。iOS 的 `bootDeviceAtIndex:`（退出游戏后下次启动必经）每次都走这条析构路径，命中率高。

**修复**：`ntimer` 新增公开 `stop()`（join 定时器线程、清空事件；`wipeout()` 顺带修成可重入——join 后重置
线程指针，避免二次 join 触发 `std::terminate`）；`~system_impl` **第一步**先 `timing_->stop()`，其后才拆
dispatcher / kernel / mem。`kernel/src/timing.cpp`、`system/src/epoc.cpp`。

### R2. `kernel_system::wipeout()` 先毁 chunks 后毁 processes（跨平台）

`wipeout()` 原顺序在 `chunks_` 清理（销毁全部 mem-model chunk）之后才 kill `processes_`；而
`~flexible_mem_model_process` 会遍历 `attachs_` 调 `attach.chunk_->mem_obj_->detach_mapping(...)`——
对 global / DLL-static 这类 kernel 持有（`mmc_impl_unq_`）、`open_to` 进进程的 chunk，其模型早已释放
→ UAF，`detach_mapping+16` @0x20/0x27 与日志完全吻合。上游 `92de98125` 修过一轮 reset 崩溃但漏了这层。

**修复**：把 `OBJECT_CONTAINER_CLEANUP(chunks_)` 移到 threads/processes 销毁**之后**（对象仍 KEEP、最后 clear）。
wipeout 期 `kern->destroy()` 本身是 no-op（`wiping_` 守卫），进程 kill 后 `get_mem_model()` 为空会跳过
`delete_chunk`，无双重销毁。`kernel/src/kernel.cpp`。

### R3. codedump 收集器持有已释放的 attached_info（运行期，退出游戏主凶）

库正常关闭走 `codeseg::detach(process_dead=false)` → `codedump_collector::add(attach_info)`（挂
`garbage_link` 缓存复用）。之后**进程退出**走 `detach(process_dead=true)` → `free_attached_data` 直接
`attaches.erase()` 释放该 info——但 `garbage_link` 仍挂在收集器链表里。同一调用链内 `attaches` 变空 →
`collector.add(seg)` → 触发阈值 `clean_impl()` → 遍历链表撞上刚释放的节点 → `free_attached_data+32` SIGSEGV，
与 DVNi 退出游戏日志逐帧吻合。这也是一处真实的**宿主堆 UAF 写**，可解释同组 FT_Load_Glyph malloc abort 的堆损坏来源。

**修复**：`free_attached_data` 在 erase 前先 `kern->get_codedump_collector().remove(info)`（幂等，
与 re-attach 路径 `codeseg.cpp:121` 既有模式一致）。`kernel/src/codeseg.cpp`。

### R4. 父子进程裸指针从不解链（运行期）

`applist_server::launch_app` 的 logon 回调遍历 `pr->get_child_processes()`；`add_child_process` 只增不减，
`detatch_from_parent()` **无任何调用点**——子进程死亡释放后，父进程 child 列表里留悬垂指针，父进程退出时
logon lambda 解引用 → SIGSEGV（BZS3 `finish_logons` 栈）。`kernel_obj::full_name` owner 链 SIGBUS 疑为同族下游。

**修复**：`process::kill` 在 finish_logons 之后解开双向链（子方 `detatch_from_parent()`，自身也从父列表摘除），
访问计数按既有配对语义回落。`kernel/src/process.cpp`。

### R5. 位图哈希用 guest 可控长度越界读（= 第一轮 B5，运行期）

`bitmap_cache::hash_bitwise_bitmap` 以 `header_.bitmap_size - sizeof(header)` 为长度读位图数据；header 在
guest 可写内存，声明尺寸可超过 fbs chunk 实际 commit 的页 → 读进保留页 SIGBUS（两份日志故障地址均页对齐）。

**修复**：`fbs_server` 新增 `readable_bytes_from(ptr, fallback)`（按 shared/large chunk 的 reserve 范围判归属、
按 committed 末端钳制，落在保留未提交区返回 0）；哈希长度取 min，另防 `bitmap_size < sizeof(header)` 下溢。
仅一次指针比较，无热点开销。`services/fbs`、`services/window/bitmap_cache.cpp`。

### R6. iOS：applist 图标扫描线程池 vs 系统重建（iOS 前端）

`rescanApps`（SwiftUI 主线程，内部 `rescan_registries` 同步等 worker 池扫图标）与
`bootDeviceAtIndex:`（control 队列 / 全局队列，重建 `symsys`）可并发：worker 还在 `create_bitmap` 里，
fbs server 连同它的 `allocator_lock_` 已被释放 → 锁已释放的 mutex（@0x1e8）。第一轮 A 类修的是分配器
**内部**竞争，这轮是**生命周期**竞争。

**修复**：`emulator` state 新增 `session_mutex`（递归）：`bootDeviceAtIndex:` / `installDevice…` /
`runLaunchAppWithUID:`（control 队列，阻塞加锁）持锁重建；`rescanApps` / `closeRunningApp`（主线程）只
**try_lock**——拿不到说明重建进行中，前者返回上次缓存列表、后者直接返回（旧会话正被整体销毁，guest 进程
随之消亡，`needs_reboot_before_launch` 已置位）。主线程绝不能阻塞等锁：boot 期间 graphics 线程会
`dispatch_sync` 回主队列挂接 CAEAGLLayer，阻塞即死锁（同 [[x7-calc-launch-deadlock]] 的锁形）。
`ios/Bridge/IosEmulator.mm`。

### R7. 音频渲染线程 vs 拆除顺序（`EKA2L1-2026-07-10-155539.ips`，退出游戏后切设备偶现）

```
崩溃线程 AURemoteIO::IOThread：SIGABRT，demangling_terminate_handler（未捕获 C++ 异常）
  ← AudioConverterFillComplexBufferRealtimeSafe（我们的 output_render_cb 输入回调内抛出）
thread 10（user-initiated 队列 = switchDevice）：卡在 AURemoteIO::Stop 的 mach_msg 等待
  ← EKA2L1 帧（bootDeviceAtIndex → ~system_impl → dispatcher shutdown → 音频流拆除）
```

**根因**：`~dsp_epoc_stream` 只调 `ll_stream_->stop()`——那是**虚拟 stop**（清 buffer、还把
`more_requested` 复位，反而重新武装 more-buffer 回调），硬件 AudioUnit 仍在跑。随后成员逆序析构：
`lock_`（mutex）先死，真正同步停硬件的 `ll_stream_`（`~dsp_output_stream_shared → stream_->stop()`，
即阻塞等待渲染周期结束的 `AudioOutputUnitStop`）最后死。窗口期内渲染回调进来：buffer 已空 → 触发
more-buffer lambda → `std::lock_guard(epoc_stream->lock_)` 锁**已析构的 mutex** → libc++ 抛
`std::system_error` → 异常穿过 CoreAudio 的 C 回调边界 → `std::terminate`。

**修复原则（不是 try/catch）**：`AudioOutputUnitStop` 同步返回即保证回调不再执行——**先停硬件流，
再析构回调可达的一切状态**。按此修四处（后三处为同族审计发现，同一窗口不同死法）：

1. `~dsp_epoc_stream`：析构体内显式 `ll_stream_.reset()`，让硬件停流发生在 `lock_`/`copied_info_`
   仍存活时（`dispatch/src/audio.cpp`）。guest 运行期 `eaudio_dsp_stream_destroy` 同样受益。
2. `~dsp_epoc_player`：notify 回调解引用 `eplayer->impl_`，而 `unique_ptr` 析构**先置空指针再跑
   deleter**，默认成员析构留下 null 解引用窗口；改为 `impl_->stop()` + `impl_.reset()`。
3. `~player_tsf`：原先先 `tsf_close(synth_)`/`tml_free` 后停流，回调期间 `tsf_render` 写已释放内存
   （宿主堆损坏的现成来源，MIDI 在 Symbian 游戏里极常见）；改为先停流（`player_tsf.cpp`）。
4. `~player_ffmpeg`：`deinit()` 先释放解码上下文、停流在基类 `~player_shared` 里更晚，且派生析构结束后
   vtable 回滚令 `get_more_data` 变纯虚；改为析构第一步先停流（`player_ffmpeg.cpp`）。

后两处跨平台生效（桌面 cubeb 后端同一回调形状）。未加 try/catch：锁死 mutex 是 UB，抛异常只是这次
恰好的死法，捕获只会把崩溃变成静默损坏；顺序正确后回调根本无机会碰到半死对象。

### R8. EGL surface 悬垂窗口指针（`EKA2L1-2026-07-10-172719.ips`，同为退出游戏后切设备）

```
崩溃线程（switchDevice 全局队列）：SIGSEGV @0xfffffffe…（野地址）
  default_data_free_func<unique_ptr<egl_surface>>   ← 容器清理销毁 surface
  （bootDeviceAtIndex → ~system_impl → dispatcher 拆除 → egl_controller::shutdown）
```

**根因**：`egl_surface` 以 `canvas_observer` 身份注册进背靠窗口并保存裸指针 `backed_window_`；
`~canvas_base` **从不通知也不清空 `observers_`**。游戏退出时其窗口随 wsclient 一起销毁，而 guest 没调
`eglDestroySurface` 的 surface 残留在 `egl_controller::dsurfaces_`；切设备拆 dispatcher 时
`~egl_surface → backed_window_->remove_canvas_observer(this)` 解引用已释放窗口 → 野地址 SIGSEGV。
运行期同样可触发（窗口先于 surface 销毁后，下一次 `make_current` 清理 dead_pending surface）。

**修复**：观察者双向解绑——`canvas_observer` 新增 `on_window_destroyed()`，`~canvas_base` 逐个弹出
observer 并通知；`egl_surface` 实现为置空 `backed_window_`（egl.cpp 所有运行期使用点本就判空）。
`services/window/classes/winuser.{h,cpp}`、`dispatch/libraries/egl/def.{h,cpp}`，跨平台生效。

### 附带：sensor 空驱动空指针（iOS 必崩点）

iOS 前端未接 sensor driver，guest 一查询传感器通道（`query_channels` / `open_channel`）即解引用 null。
补空指针守卫：查询回 0 通道、开通道回 `error_not_supported`。`services/src/sensor/sensor.cpp`。
后续接真实 CoreMotion 驱动时此守卫仍然成立。

---

## 未动的项

- `CziX` 崩溃点名义上的 `SwiftUICore ViewGraphDisplayLink.asyncThread` 帧来自聚类代表帧，逐份日志拆开后
  全部落在 R2 / sensor 两个根因，无独立的 SwiftUI 侧问题（第一轮 C 类结论不变）。
- 第一轮 B4（GL 客户端顶点数组越界读）本轮未再出现，维持「复采后再定」。

## 验证

- `EKA2L1_IOS_CONFIGURATION=Release scripts/build_ios.sh simulator` 通过。
- `scripts/ios_regression_test.sh`（Release，含 Final Battle 退出 → Calculator 再启动的
  `bootDeviceAtIndex:` 重建路径，即本轮主要崩溃现场）**8/8 PASS**，模拟器日志无 panic / 访问违例。
- `scripts/ios_regression_test.sh angrybirds`（X7 / Symbian^3，覆盖 kernel 改动对 S^3 的影响）**5/5 PASS**。
