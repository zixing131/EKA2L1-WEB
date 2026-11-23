# iOS TestFlight 崩溃三分类与修复（26.7.0 / build 260713）

针对 TestFlight 组织者面板导出的 `Crashes.zip`（25 个崩溃点、72 份日志，多数来自 iOS 27.0 beta 设备）逐点归因。
结论：**主因是一处上游并发缺陷（applist 后台线程池并发调用 `fbs_server::create_bitmap`），已修复**；其余分为
「其它 EKA2L1 侧待跟进」与「系统 / iOS beta / SwiftUI 侧、非本项目代码」两类。

已落地：**A 类**（`create_bitmap` 分配器竞争）、**B1**（音频 DSP 回调跨线程 UAF）、**B2**（property 析构期 completion）、
**B3**（退出 teardown 顺序）。其余（B4/B5 图形越界读、B6 freetype 堆损坏等）给出归因与建议，待上线后按新数据再定，
避免在图形热点路径上加投机性 patch（见 [[ios-metal-angle-plan]] 相关热点约束）。

---

## A 类（已修复）：`create_bitmap` 分配器数据竞争 —— 主因

**崩溃点**：`acOcXESsIusjpPb_xtJF`、`Bun9B7RLrTh69C40vCkEce`、`BfrLJ_TCel9uJnRKSu5mdE`、`BaCGvMVvBmqfwWPQkwKYGZ`
（另 `COcpflTHlIBbtdQ3mQjlxY` 之外的 rescan 主线程 `SIGSEGV @0x160` 现场同源）。合计 ~5 个点，是导出集里出现最多的一族。

**现场**：
```
0  fbs_server::create_bitmap(...)                       ← SIGSEGV @0x160
1  read_icon_data_aif(...)::$_0::operator()
2  read_icon_data_aif(...)
3  applist_server::load_registry_oldarch(...)
4  BS::thread_pool::submit_loop worker lambda
5  BS::thread_pool::worker(...)
   (worker 线程 11/13/18/9…)
```

**根因**：`applist_server::rescan_registries` 用 `loading_thread_pool_.submit_loop(...)` 把注册表加载
（含 `read_icon_data_aif` → `serv->create_bitmap`）并行分发到工作线程池（上游 `c3ea261e9` 引入的并行化，跨平台生效）。
但 `fbs_server::create_bitmap` / `free_bitmap` 以及通用/大块数据分配都直接改写共享的两个宿主分配器
（`shared_chunk_allocator` / `large_chunk_allocator`，`common::block_allocator`，**无任何内部锁**）。多个 worker 同时
`create_bitmap` → free-list 破坏 → 后续 `bitwise_bitmap`/`fbsbitmap` 拿到近 null 指针，`+0x160` 字段解引用即崩。
`make_new`（对象容器）本身有锁，故唯一未同步的共享态就是这两个 chunk 分配器。

**修复**（本仓库）：给 `fbs_server` 增加 `std::recursive_mutex allocator_lock_`，在所有会改写这两个分配器的入口加锁：
`create_bitmap` / `free_bitmap`（含惰性 `initialize_server` 检查，避免双重初始化竞争）、`allocate_general_data(_impl)` /
`free_general_data_impl` / `allocate_large_data` / `free_large_data` / `load_data_to_rom`。递归锁是因为
`create_bitmap` 内部会再调 `allocate_general_data`。

**性能**：这些入口都是 guest 侧 FBS IPC 事件（建位图 / 建字体），**不在每帧合成热点上**（每帧合成走 driver 侧
`bitmap_cache` / `font_atlas`，与该 chunk 分配器无关）；唯一略热的 `allocate_general_data`（字形缓存）仅 EKA1 路径且按字体一次。
默认 `fbs_enable_compression_queue=false`，压缩线程不跑，稳态下该锁始终无竞争，成本≈一次未争用原子操作，可忽略。
顺带把（若开启压缩队列时）压缩线程与主线程对分配器的既有潜在竞争也一并覆盖。

**验证**：`scripts/build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**（Final Battle 进游戏、Calculator 全流程，无 guest crash）。

---

## B 类（其它 EKA2L1 侧）

按「根因清晰度 / 影响」排序：

### ✅ B1. 音频 DSP 回调跨线程 UAF（已修复）（`CMJEhntwPCHcDm3mn6ehbY`、`DLHVi3KFXXfjOzCKQJRoj1`）
```
0  eaudio_dsp_stream_create_impl(...)::$_0  /  notify_info::complete(int)   ← SIGSEGV @0x…020
1  dsp_output_stream_shared::data_callback(short*, unsigned long)
2  output_render_cb(...)   ← CoreAudio 渲染线程 (thread 16/17)
```
**根因**：`audio.cpp` 里 DSP 流 `more_buffer` 回调（及播放器 `notify_any_done` 回调，同型）在 **CoreAudio 渲染线程**
上调 `info.complete()`，而 `notify_info::complete` 会解引用 `info.requester`（guest `kernel::thread*`）。当 guest 侧
请求线程/进程在通知触发前被销毁（app 退出、player/stream 死亡），`requester` 成悬垂指针 → UAF。故障地址正是
`requester + 小偏移`。原回调更糟：还先靠 `info.requester->get_kernel_object_owner()` 去**取** `kern`，即崩在拿锁之前。

**修复**：把稳定的 `kernel_system*`（`sys->get_kernel_system()`，生命周期覆盖整个会话）捕获进两个回调 lambda，不再经
`requester` 取 kern；新增 `complete_audio_notify_if_alive(kern, info)`：在 `kern->lock()` 下先按指针在
`kern->get_thread_list()`（线程销毁时会从中 `erase`）里校验 requester 仍存活，存活才 `complete()`，否则把 `info.sts=0`
丢弃这条陈旧通知，绝不解引用悬垂指针。锁序与原实现一致（回调内 `kern->lock()`），未引入新锁序。
验证：`build_ios.sh simulator` 通过；`ios_regression_test.sh` **8/8**（Final Battle 含音频路径无回归）。

### ✅ B2. `notify_info::complete` 于内核对象析构期（已修复）（`CG2SBgy4y_i7gAaLgoWniS`）
```
notify_info::complete → property::cancel → property_reference::~property_reference
  → kernel_system::destroy → kernel_obj::decrease_access_count
```
**根因**：property 引用因句柄关闭（`decrease_access_count → destroy`）析构时，`~property_reference → cancel → property::cancel`
对订阅里的 `notify_info` 调 `complete(error_cancel)`，而订阅者线程可能早已退出（未取消订阅就结束）——`complete` 解引用
悬垂的 `requester` → UAF。与 B1 同属「completion 目标已失效」家族。
**修复**：`property::cancel` 与 `property::notify_request` 在 `complete()` 前用新增的 `kernel_system::is_thread_alive(requester)`
（按指针查 `threads_`，线程析构会从中 erase）校验订阅者存活；不在则只从订阅队列移除、不 complete。B1 的音频回调 helper
也改用同一 `is_thread_alive` 原语。验证：`ios_regression_test.sh` **8/8**。

### ✅ B3. 退出期 teardown 顺序（已修复）（`CkaoCnpe2ABfCUUMGe8Daj`，3 份日志）
```
~system_impl → kernel_system::wipeout → session::destroy → session::detatch
  → semaphore::signal → thread_scheduler::dewait   ← SIGSEGV
```
**根因**：`~system_impl → kernel_system::wipeout`（已置 `wiping_=true`）先销毁 `sessions_`，`session::detatch` 为
「完成在途消息」调 `msg->own_thr->signal_request()`（→ `request_sema->signal → thread_scheduler::dewait`），但整个调度器/
线程正在拆除，`dewait` 触到已失效的调度状态 → 原生 SIGSEGV。
**修复**：`session::detatch` 的消息完成/线程 signal 块加 `!kern->is_wiping()` 守卫（`kernel_system` 新增 `is_wiping()`）——
仅正常关闭 session 时才需要唤醒客户端线程；wipeout 期这些请求无需完成，跳过 signal 即可，`msg->unref()` 清理照常。
验证：`ios_regression_test.sh` **8/8**。

### B4. GL 客户端顶点数组越界读（`BFolXPfRCqdvkXR0lPEpEs`、`CsziXJd0iuRo9BQzl67vD`）
```
_platform_memmove ← SIGBUS @0x…ffe6 (页边界)
graphics_buffer_pusher::push_buffer
egl_context_es_shared::retrieve_vertex_buffer_slot
egl_context_es1::prepare_for_draw → gl_draw_elements_emu
```
`retrieve_vertex_buffer_slot`（`gles_shared.cpp`）中 `buffer_obj_==0`（客户端数组）的**可预测索引**分支直接 memcpy
`stride*vcount` 字节，缺少 unpredictable 分支才有的分页连续性 clamp；当 `glDrawElements` 索引越过实际数组、或数组恰好
贴着 chunk 末页时，读入未映射页 → SIGBUS。属 guest 驱动的越界读，防御式修法需在**热点 GL 绘制路径**加 clamp，
有渲染回归/性能风险，需回归各游戏画面后再动。

### B5. 位图 cache 哈希越界读（`JpXxzZFlio65lJtOjfijY`、`BZS3HhjnQgmS5EP5KfkwiM`）
```
_platform_memmove / XXH64_update
bitmap_cache::hash_bitwise_bitmap   ← 读 header_.bitmap_size - sizeof(header) 字节
bitmap_cache::add_or_get → canvas_base::add_draw_command → gdi_blt_masked
```
窗口合成时用**guest 可控的** `header_.bitmap_size` 作为哈希长度读位图数据。`BZS3` 故障地址**页对齐**（`0x134f9c000`），
是经典「读越过有效分配落入守护页」——guest 自建位图头的 `bitmap_size` 大于实际分配；`JpX` 故障地址非对齐，更像指针本身被破坏
（可能是 A 类竞争的下游）。可加「按所在 fbs chunk 边界 clamp 哈希长度」的安全防护，但 `add_or_get`/`hash` 在**每帧合成热点**上，
需权衡；且部分实例可能随 A 类修复消失，建议 A 类上线后复采数据再定。

### B6. FreeType 取字形时 malloc abort（`DVNiQ9Mf2AhRA95jbeZcO6`）
```
libsystem_malloc abort (SIGABRT)
FT_Load_Glyph → freetype_font_adapter::get_glyph_atlas → font_atlas::draw_text
  → animation_scheduler::invoke_due_animation
```
freetype 分配时检出**宿主堆**损坏。fbs chunk 分配器不碰宿主 malloc 堆，故非 A 类直接下游；疑似字体路径的并发访问或别处
堆溢出。单点、根因不清，建议观察 A 类修复后是否复现。

### B7. `p2Yk2z-nW_jn_yk58J4ko`（未符号化）
worker 线程 11、短 pthread 栈、`SIGSEGV @0x78`。形态与 A 类 worker 一致，疑同族；待符号化/新数据确认。

### B8. Smoke 测试路径 spdlog（`COcpflTHlIBbtdQ3mQjlxY`）
```
spdlog::logger::log_it_ ← PAC failure
CpuSmokeBridge.runWithBackend → InterpreterMainLoop
```
CPU smoke 自检路径的日志崩溃，属诊断/自检链，用户可感知度低。

---

## C 类（系统 / iOS 27 beta / SwiftUI-UIKit，非本项目代码）

多数崩在 iOS 27.0 beta（`24A5380h`）上，属 OS/框架 beta 不稳定，非 EKA2L1 代码：

| 崩溃点 | 顶栈归属 | 说明 |
|--------|----------|------|
| `ClYzbTC4OPxXFOvXGrtbgL` | SwiftUI NavigationStack 拆除 | CODESIGNING Invalid Page（beta） |
| `KfVjgSSLf8PgnPYAS2ZMq` | UIKit `UIRemoteKeyboardWindow` `objc_release` | 键盘窗口拆除 |
| `CdWZel7Xnia1-xUAReR2FA` | AudioAnalytics / CoreHaptics XPC | 系统遥测 |
| `CfszbbEbd6Quo6aSERj1zA` | QuartzCore `IOSurfaceCreate` / `CFHash` | 表面分配 SIGTRAP |
| `B8zM5Dpw2-W9q1a_ikaK7K` | SwiftUI `Button` assignWithCopy 析构 | View graph 内部 |
| `BUSvO1Wa3OmYCQ1m0y7wLm` | ColorSync / CoreGraphics / vImage | 图片解码 |
| `CbXJ3MievpXVcQpWyz5mj5` | CoreHaptics `CHMetrics` | 触感遥测 |
| `JFsJuzDnFidvBr5AcNv0m` | CoreGraphics `CGGStateSetFillColor` / CABackingStore | 图层背板 |
| `B51Y3hXKYd9iazSYT-pUO7` | SwiftUI `LocalizedStringKey`/`Text` 相等比较 | 文案 diff |
| `BRAkL8wkEQkscNGfA9Fun7` | objc `lookUpImpOrForward` fatal | 运行时 |

其中 SwiftUI（`ClY`/`B8z`/`B51Y`）与我们的 UI 代码相邻，可留意但当前证据指向 iOS beta 框架自身；建议随正式版 iOS 27 复核。

---

## 后续建议

1. A/B1/B2/B3 修复上线后，重采一版 TestFlight 崩溃，核对 B5/B6/B7（疑似 A 类下游）是否消失。
2. B4/B5 若仍复现，再在图形路径加**只在越界时生效**的边界 clamp，并跑各游戏画面回归。
3. B6（freetype 宿主堆损坏）需在新数据里确认是否随 A 类消失，否则排查字体路径并发访问。
