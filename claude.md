# EKA2L1 WASM Web 版调试记录（续）

> 对话时间：2026-05-13 ~ 2026-05-14
> 接续 [cursor.md](./cursor.md)，继续修复 WASM 版的运行时崩溃 / 死锁 / 持久化问题
> 目标：让 EKA2L1 在浏览器中可启动、可装 ROM、可列出 app、可点击运行、可持久化

---

## 错误链与修复过程

### 错误 10：`Uncaught RuntimeError: memory access out of bounds` in ntimer 线程

**现象**
点击 Install ROM 后崩溃，调用栈：
```
__thread_proxy<ntimer::reset()::$_0>(void*) (eka2l1.wasm:0x360b08)
  ← __pthread_create
  ← eka2l1::system::set_device(unsigned char)
  ← wasm_init_with_rom
```

**初步推断**
默认 `INITIAL_MEMORY` 仅 16MB，timer 线程创建时栈/堆分配越界。

**修复**（`src/emu/web/CMakeLists.txt`）
```
"SHELL:-s INITIAL_MEMORY=268435456"   # 16MB → 256MB
```

---

### 错误 11：SDL 互斥锁 `memory access out of bounds`

**现象**
内存增大后 ntimer 线程不再崩，但 `main_loop()` → `SDL_PollEvent` → `SDL_LockMutex` → `__pthread_mutex_trylock` 崩溃。

**根因**
Emscripten 默认 pthread 栈仅 64KB。Worker 线程（ntimer）栈溢出后污染邻近堆内存，恰好覆写了 SDL 全局事件队列的 mutex 指针 `SDL_EventQ.lock`。后续主线程 `SDL_LockMutex` 解引用坏地址 → WASM 越界陷阱。

**修复**
```
"SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=2097152"   # 64KB → 2MB
"SHELL:-s PTHREAD_POOL_SIZE=8"                  # 预创建 worker 数
```

---

### 错误 12：同样的 `memory access out of bounds` 在多种线程类型上重现

**现象**
增加 worker 栈后，`ntimer::reset()::$_0` 和 `BS::thread_pool::worker_thread` 仍在 `__thread_proxy` 入口处的同一个 WASM offset 崩溃。地址固定、与内存大小无关。

**诊断**
加 `-s ASSERTIONS=1` 重新构建，浏览器控制台暴露真正错误：

```
Aborted(Stack overflow! Stack cookie has been overwritten at 0x0027ce10,
        expected hex dwords 0x89BACDFE and 0x2135467,
        but received 0x1f504061 0xf6218f81)
```

**根因**
**主浏览器线程的 shadow stack 溢出**（默认仅 64KB），cookie 在 0x0027ce10（≈2.6MB 地址）被覆写。
`init_emulator()` → `set_device(0)` → `kernel_system::reset()` 复杂 C++ 模板调用链超过 64KB 栈空间，污染相邻全局数据。Worker 线程的"越界"是次级现象（主线程栈溢出腐坏了它们的入口参数）。

**修复**（`src/emu/web/CMakeLists.txt`）
```
"SHELL:-s STACK_SIZE=8388608"          # 主线程 64KB → 8MB
"SHELL:-s INITIAL_MEMORY=536870912"    # 256MB → 512MB
"SHELL:-s MAXIMUM_MEMORY=2147483648"   # 1GB → 2GB
"SHELL:-s PTHREAD_POOL_SIZE=32"        # 8 → 32（线程池耗尽警告）
```

---

### 错误 13：`emscripten_set_main_loop_timing: Cannot set timing mode... a main loop does not exist!`

**现象**
启动时 SDL `SDL_GL_SetSwapInterval(1)` 调用 `emscripten_set_main_loop_timing`，但此时 `emscripten_set_main_loop` 尚未注册。

**修复**（`src/emu/web/src/main.cpp`）
```cpp
// WASM 下跳过：emscripten_set_main_loop 用 requestAnimationFrame 自带 vsync
#ifndef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(1);
#endif
```

> 注：另一处同名警告来自 SDL `SDL_Init` 内部，无法避免，无害。

---

### 错误 14：`Blocking on the main thread is very dangerous` + setTimeout 卡 1.4 秒

**根因（第一处）**
页面重载、IDBFS 已有设备数据时：
1. `init_emulator()` 检测到设备存在 → 调 `set_device(0)` → **timer 线程启动**
2. `wasm_init_with_rom()` 再次调 `set_device(0)` → `ntimer::reset()::wipeout()` → `timer_thread_->join()` → **阻塞主浏览器线程**（Emscripten pthreads 主线程不允许 `Atomics.wait`）

**修复**（`src/emu/web/src/main.cpp`）
移除 `init_emulator()` 里的 `set_device()`，由 `wasm_init_with_rom()` 统一调用一次。

---

### 错误 15：阻塞仍在 + 已安装的 app 不显示

**根因（第二处）**
`applist_server::rescan_registries()` 用 `BS::thread_pool::submit_loop()` 并行加载注册表，然后 `load_registry_task.wait()`——这在浏览器主线程上**死锁**：主线程被 `Atomics.wait` 挂起后无法处理 worker 的完成消息，wait 永不返回，app 列表永远为空。

**修复**（`src/emu/services/src/applist/applist.cpp`，两处）
```cpp
#ifdef __EMSCRIPTEN__
    for (std::size_t idx = 0; idx < register_file_paths.size(); idx++) {
        // 同步加载，绕过线程池
        bool modified = kern->is_eka1()
            ? load_registry_oldarch(io, register_file_paths[idx], drv, lang)
            : load_registry(io, register_file_paths[idx], drv, lang);
        if (modified) global_modified = true;
    }
#else
    auto load_registry_task = loading_thread_pool_.submit_loop(...);
    load_registry_task.wait();
#endif
```

---

### 错误 16：初始化完成后整个程序卡住

**根因（第三处）**
`thread_scheduler` 启用 `cpu_load_save` 时，无线程可运行就调 `idle_event.wait()`。这个调用在 `system_impl::loop()` 持有 `mut` 期间发生，导致**经典死锁**：

```
main thread:  loop() 持有 mut → crr_thread() → 无线程 → idle_event.wait() 永远等
timer thread: 回调需要内核状态 → 需要 mut → 被 main 阻塞
              → 永远无法 idle_event.set()
```

`cpu_load_save` 是桌面优化（调度器空闲休眠），但 WASM 上调度器跑在主线程，不能阻塞。

**修复**（`src/emu/system/src/epoc.cpp` `system_impl::startup()`）
```cpp
#ifdef __EMSCRIPTEN__
    // 调度器跑在主线程，不能 idle_event.wait()
    conf_->cpu_load_save = false;
#endif
```

---

### 错误 17：点击 app 后黑屏 + 大量 `__syscall_mprotect` 警告

**根因**
`common/src/virtualmem.cpp` 的 `commit()` / `decommit()` / `change_protection()` 直接调 `mprotect()`。WASM 上 Emscripten stub `mprotect` 返回 -1，函数返回 `false` → Symbian 内存管理器认为内存提交失败 → app 代码段无法加载 → 黑屏。

**修复**（`src/emu/common/src/virtualmem.cpp`）
```cpp
bool commit(...) {
#if EKA2L1_PLATFORM(WIN32)
    ...
#elif EKA2L1_PLATFORM(WASM)
    // WASM 没有页级保护，内存始终可访问
    (void)ptr; (void)size; (void)commit_prot;
#else
    if (mprotect(...) == -1) return false;
#endif
    return true;
}

// decommit / change_protection 同理

bool is_memory_wx_exclusive() {
#if EKA2L1_PLATFORM(UWP) || EKA2L1_PLATFORM(IOS) || EKA2L1_PLATFORM(ANDROID) || EKA2L1_PLATFORM(WASM)
    return true;   // 走 W^X 兼容路径
#else
    return false;
#endif
}
```

---

### 错误 18：IDBFS 已持久化，但刷新后 app 列表丢失

**根因**
错误 14 修复后，`init_emulator()` 不再自动 `set_device()`。所以即使 IDBFS 数据已从 IndexedDB 恢复到 `/eka2l1/`，刷新后 `wasm_get_state()` 永远是 0，UI 不知道有设备，也不会触发初始化。

**修复**（`src/emu/web/shell.html` postRun 回调）
```javascript
// 用空字符串调 wasm_init_with_rom：
// - 若 device_manager 已有设备（IDBFS 恢复出来的）→ 跳过安装、直接 set_device(0) → 返回 0
// - 若没有设备 → ROM 路径无效返回 -3，提示用户加载 ROM
var result = module.ccall('wasm_init_with_rom', 'number',
    ['string', 'string'], ['', '']);
if (result === 0) {
    updateStatus('green', 'Device loaded from storage');
    refreshAppList();
} else {
    updateStatus('yellow', 'Load a ROM/RPKG to begin');
}
```

---

### 错误 19：打开页面立刻 `memory access out of bounds` in `spdlog::logger::log_it_`

**现象**
刷新页面，splash 还没消失就报：
```
eka2l1.wasm:0x370b4a Uncaught RuntimeError: memory access out of bounds
  at spdlog::logger::log_it_(...)
  ← logger::log_<char[70], int, char const*, char const*&, char const*>(...)
  ← wasm_init_with_rom (eka2l1.wasm:0x5f7ca)
  ← Object.ccall
  ← eka2l1.html:699  (postRun setTimeout)
```

**根因**
`moduleConfig.noInitialRun = true` 让 Emscripten 不自动调 `main()`，必须由 JS `callMain()` 触发。但 **postRun 在 callMain 之前就跑了**（postRun = "module 启动完毕"，与 main 是否运行无关）：

```
正确顺序应是: WASM ready → postRun → IDBFS mount/sync → callMain → main() → setup_log → ...
实际顺序却是: WASM ready → postRun(里面的 setTimeout 500ms) → ... → callMain
```

错误 18 的修复把 `wasm_init_with_rom("", "")` 放进了 postRun 的 setTimeout 中，结果该调用在 `main()` 跑之前就触发了——这时 `eka2l1::log::spd_logger` 还是空 `shared_ptr`。`LOG_INFO` 宏先过 `filterings->is_passed(...)` 检查（filterings 也未初始化但 reset 状态恰好让 is_passed 返回 true，或栈布局蒙混过关），然后调 `spd_logger->info(...)` 解引用 null shared_ptr → WASM trap。

也就是说错误 18 不能放 postRun 里，必须放到 callMain 之后。

**修复**（`src/emu/web/shell.html`）
- 把 postRun 简化为只更新 UI 进度条，**不再调** `wasm_init_with_rom`
- 把设备恢复逻辑挪到 `startMain()` 里的 `mod.callMain([])` **之后**（main() 抛 unwind 是预期的，要 try/catch 掉以让流程继续）

```js
function startMain() {
    try { mod.callMain([]); }
    catch (e) {
        if (!(e === 'unwind' || e.name === 'ExitStatus')) {
            console.error('main() threw unexpected error:', e);
        }
    }
    // 此时 spd_logger 已就绪
    var result = module.ccall('wasm_init_with_rom', 'number',
        ['string', 'string'], ['', '']);
    ...
}
```

**第二层防御**（`src/emu/web/src/main.cpp`）
在 `wasm_init_with_rom` 入口加 spd_logger null-check，即使 JS 端再有 bug 误在 main 之前调用，C 端也不会崩：
```cpp
if (!eka2l1::log::spd_logger) {
    eka2l1::log::setup_log(nullptr);
    eka2l1::log::toggle_console();
}
```

---

## 涉及文件汇总（本轮新增/修改）

| 文件 | 改动 |
|---|---|
| `src/emu/web/CMakeLists.txt` | `INITIAL_MEMORY=512MB`、`MAXIMUM_MEMORY=2GB`、`STACK_SIZE=8MB`、`DEFAULT_PTHREAD_STACK_SIZE=2MB`、`PTHREAD_POOL_SIZE=32`、`ASSERTIONS=1` |
| `src/emu/web/src/main.cpp` | `SDL_GL_SetSwapInterval` 加 `#ifndef __EMSCRIPTEN__`；移除 `init_emulator()` 里的 `set_device()` |
| `src/emu/web/shell.html` | postRun 只更新进度；设备恢复 (`wasm_init_with_rom("","")`) 挪到 `startMain()` 里 `callMain()` 之后，并 try/catch 掉 `unwind` |
| `src/emu/web/src/main.cpp` | `wasm_init_with_rom` 入口给 `spd_logger` 加 null-check，必要时自动 `setup_log()` |
| `src/emu/services/src/applist/applist.cpp` | WASM 下 `rescan_registries` 与 `on_drive_change` 同步加载，绕过 `BS::thread_pool::submit_loop().wait()` |
| `src/emu/system/src/epoc.cpp` | WASM 下 `system_impl::startup()` 强制 `conf_->cpu_load_save = false` |
| `src/emu/common/src/virtualmem.cpp` | WASM 分支跳过 `mprotect`；`is_memory_wx_exclusive()` 返回 `true` |

---

## 关键经验

1. **Emscripten + pthreads + 主浏览器线程禁止 `Atomics.wait`**：所有 `join()` / `future::wait()` / `condition_variable::wait()` / 阻塞的 `mutex::lock()` 在主线程上都会死锁。需要在 WASM 上把它们换成同步执行或非阻塞轮询。

2. **诊断技巧**：`-s ASSERTIONS=1` 给出栈 cookie 地址 + 期望值/实际值，比单看 "memory access out of bounds" 信息量大得多。

3. **栈大小是默认 64KB**，对于复杂 C++ 模板调用链远远不够。WASM 主线程需要 8MB+，worker 至少 2MB。

4. **`cpu_load_save` 是桌面调度器优化**，依赖"调度器在自己的线程里休眠"模型，与 WASM 单主线程模型根本冲突，必须关掉。

5. **WASM 没有 mprotect**：所有依赖页级保护的代码必须走 W^X 兼容路径或返回成功，否则模拟器以为内存提交失败。

---

## 构建命令

```bash
cmake --build build_wasm_test --target eka2l1_wasm -j4

# 测试
cd build_wasm_test/bin
python3 ../../src/emu/web/serve.py 8080 .
```

---

## 仍未解决 / 待验证

- [ ] mprotect 修复后黑屏是否解决（用户验证中）
- [ ] 持久化是否在刷新后正常恢复（用户验证中）
- [ ] `__syscall_mprotect` 警告 = Emscripten 层调 mprotect，**不是** EKA2L1 调的；现在 EKA2L1 这边不调了，Emscripten 那边的警告可能来自 libc 内部（如线程栈分配），数量应大幅减少
- [ ] 长时间运行时 CPU 性能：dyncom 解释器在 WASM 上很慢，主循环每帧能跑的 ticks 数有限，复杂 app 可能很卡

---

## 第 28 轮（2026-06-12，commit 85fac43fa）：RGB565 检测误伤双案 + 占位字体

### 错误 20：snakes.sis（3D 贪吃蛇）花屏 —— 565 检测被启动白屏锁死

**现象** 之前正常，现在菜单/游戏全是红蓝噪点竖纹。guest 帧缓冲转储完全正常 → 病在呈现路径。

**定位方法** dev-test 每秒转储 chunk，离线复算启发式：t=1s 的启动过渡帧 99.4% 纯白
0xFFFFFFFF —— 白色满足 `hi16==lo16`，而 screen.cpp 的 DSA 层 565 检测只排除了 0
（update_bitmap 层的副本排除了白，screen.cpp 这份漏了）→ 连续 2 帧白屏即永久锁死
verdict=-1 → 之后所有正常 32bpp 帧被按 565 拆成两个像素。蓝色竖纹 = 灰启动画
0x404040 拆成 0x4040（暗红）/0x0040（近黑）再被 WASM 上传层 R/B 交换。

**修复**（screen.cpp + graphics_driver_shared.cpp 两层）
- 排除 0xFFFFFFFF 与四字节全等（纯灰）槽位（见错误 21）
- 改三态判定：looks-565 / looks-32bpp / 空帧不计（白屏不再推进判定）
- 锁可逆：锁死后继续监视原始帧，连续 2 个确凿 32bpp 帧自动解锁
- screen.cpp 的转换输出按 WASM R/B 交换预先反转字节序（修真 565 内容的偏色）

### 错误 21：字体"彩色碎块"残留真根因 —— 565 检测吃掉字体图集

**现象** 第 27 轮修复后管家对话框正常，但 19px 副行/图标标签仍是黑条+彩色碎屑；
X-plore 中文仍碎。

**定位** 探针证明 19px 文本逐字回退路由完全正确（Droid 回退图集 ok=true），但屏幕
仍碎 → 放大像素：黑条内是水平彩色条纹 = 纹理数据被 565 转换的杂讯。**字体图集是
灰度 RGBA（每像素 v,v,v,v）→ uint32 = 0xVVVVVVVV → hi16==lo16 恒成立 → update_bitmap
层把图集纹理判成 565 帧并转换！** 21px 图集因采样非零数恰 ≤1000（inconclusive）逃过，
19px 中招；S60SC 点阵图集只有 0/纯白（被排除）故当年正常、第 26 轮换 Droid 灰度 AA
后恶化 —— 所有历史症状归一到这一个根因。

**修复** 四字节全等（纯灰）不计入 565 证据：真实 565 翻倍匹配是 0xXYXY（X≠Y），
灰度图集是 0xVVVV。两层检测同步加固。

### 错误 22：占位字体骗过 does_glyph_exist（管家部分字体黑盒的前置原因）

西文 5320 ROM 的 Heisei Kaku Gothic 等字体 **cmap 声称覆盖 CJK 但所有汉字轮廓都是
同一个占位盒**（省空间阉割），`does_glyph_exist` 返回 true → 第 27 轮的逐字回退永不
触发 → 主图集画出一排占位盒。两个探针字（中/蓝）也被它骗过（有 中 缺简体专属的 蓝
→ 旧检测因"两字必须都在"而跳过）。

**修复**（font_store）`is_cjk_placeholder_font`：渲染 中/体/国/日/蓝 中该字体声称的
探针字（≥2 个），**全部逐字节相同 ⇒ 占位字体**（真字体不可能两字形 bit 级一致），
按 (adapter,face) 缓存；`can_really_draw` = cmap 有 + 非占位，接入 draw_text /
rasterize_glyph / seek 三处。

### 附带加固

- font_atlas LRU 改真"移到前端"去重（原 insert+尾部砍断会积累重复、静默驱逐仍在
  characters_ 里的字形）；重建路径 `characters_.clear()` 后只回填实际重打包的字形
  （原先残留陈旧坐标 → churn 后画出图集错误区域 = 碎块），打包失败按容量减半重试。
- wserv 图集 freetype 渲染 LCD→NORMAL 灰度 AA（LCD 子像素在 CJK 密集笔画上呈彩边）。

### 验证（headless dev-test 截图）

snakes 菜单+3D 游戏 ✓、天地道转置+中文对话 ✓、管家标题/对话框/19px 副行/全角标点 ✓、
X-plore 英文界面+目录浏览 ✓。

### 已知遗留

- **X-plore 展开含 CJK 文件名的盘符 → 主线程永卡 requestSema**（某异步请求未
  complete；ASCII 名+相同扩展名正常 → 纯 CJK 文件名触发；与字体无关）。
  复现：`dev-test.html?game=xplore&cjkfiles=2&script=lsk@6,down@10,down@13,right@16`。
- 测试基建：build_wasm_test/bin/dev-test.html 重写（?game=snakes/guanjia/xplore/2048、
  ?script=KEY@sec 定时按键、?cjkfiles=1/2/3/4 预置 E: 测试文件）；运行脚本
  /tmp/eka2l1-test/run-test.sh（serve 8086 + headless Chrome + 截图/帧缓冲提取）。
  build_wasm_prof 是旧构建（Jun 11），本轮一直用 build_wasm_test。

### 教训

1. **指纹检测必须考虑"合法内容撞指纹"**：hi16==lo16 同时命中 565 翻倍、纯白、纯灰
   —— 而灰度恰是字体图集的全部内容。加判别时先问"什么正常数据也长这样"。
2. **永久锁死的启发式必须可逆**：任何"连续 N 帧判定后锁死"的检测都要留解锁路径，
   否则一个启动过渡帧就能毒化整个会话。
3. **同一启发式复制两份 = 漏修两次**：screen.cpp 与 update_bitmap 层的白色排除
   不一致就是本轮第一案。
4. **cmap 不可信**：字体声称有字形 ≠ 字形是真的。渲染同一性（两字 bit 级相同）是
   廉价且零误报的占位字体判据。

---

## 第 29 轮（2026-06-12，commit 6c4d01dd9）：X-plore CJK 客户端字形乱码

承接第 28 轮：管家（wserv 图集路径）已修，但 X-plore 中文菜单/盘符仍是满屏
白点乱码。用户在浏览器把 X-plore 设成中文并保存到 IndexedDB 后复现。

### 定位：服务端字形是对的，病在客户端消费

加探针 dump U+6587(文) 的服务端原始字节（`0404C3FF04210B33...`），离线按
Symbian 单色 RLE 解码 → **画出完美的「文」字**。即字形数据、RLE 编码、Droid
回退全部正确。屏幕乱码 ⇒ 客户端 rasterize_glyph 路径消费错误。

**两条渲染路径再次分野**：管家 CJK 走 wserv `get_glyph_atlas`（font_atlas，
类型自洽，第 28 轮已修）；X-plore CJK 走**客户端 rasterize_glyph IPC**（app 自己
把字形 blit 进离屏位图）。

### 根因 1（决定性）：字形位图类型不匹配

guest 创建 font 时把 `font.glyph_bitmap_type` 锁定为**绑定字体** adapter 的
`get_output_bitmap_type()`——freetype/STB 都声明 `antialised_glyph_bitmap`(8bpp)。
之后该 font 的每个字形都按此类型解码，**包括借自别的字体的回退字形**。但
freetype `get_glyph_bitmap` 对 ppem≤15 小字形强制返回**单色 RLE**（第 24 轮为低
色深离屏加的）→ guest 把单色 RLE 字节当 8bpp 读 = 白点乱码。

- Latin 正常：绑定字体自渲染，类型自洽；只有 CJK 走回退才暴露不一致。
- "S60SC 好换 Droid 坏"在客户端路径的解释：S60SC 内嵌点阵 FreeType 直接返回
  1bpp，恰是 guest 期望的格式之一；纯轮廓 Droid 被强制 mono 后与 AA 期望冲突。

**修**：`rasterize_glyph` 把绑定字体声明的输出类型（`font->of_info.adapter->
get_output_bitmap_type()`）作为**请求**传入 `get_glyph_bitmap`；freetype 据此决定
mono/AA——请求 AA 永不强制 mono，内嵌点阵字体遇 AA 请求则把 1bpp 展开成 8bpp。
保证一个 font 的所有字形（含跨字体回退）类型一致。

### 根因 2：度量宽误用设计值

MONO/AA 两分支都把 `character_metric.width/height` 设为 `glyph->metrics`（设计
bbox），而 RLE/8bpp 数据按 `bitmap.width/rows` 编码，轮廓字体二者差 ±1px →
客户端按错误行宽解码再次错位。**修**：width/height 改用实际位图尺寸、bearing
改用 `bitmap_left/top`，对齐 GDR 适配器（`metric.width = target_width`）的做法。
（单独修根因 2 不够——视觉无变化，因为客户端实际用 rasterized_width；但保留此修
是正确性必需，且对其他消费者有益。）

### 验证

X-plore 菜单/盘符中文全清晰（文件/查找/编辑/工具/退出、可用 1.0GB、选择/取消）；
安全管家全界面、天地道中文对话、snakes 英文菜单均正常（英文更锐利=AA 渲染附带
收益）。**X-plore 之前的"展开 CJK 文件名盘符卡 requestSema"遗留也未再现**（本轮
设中文后菜单与盘符浏览均正常）。

### 教训

1. **字形 bug 先分离服务端产物与客户端呈现**：dump 字节离线解码，确认服务端对了
   就别在服务端绕圈。本轮服务端 100% 正确，纯客户端消费 bug。
2. **glyph_bitmap_type 是 per-font 契约**：guest 按绑定字体声明的单一类型解码所有
   字形，回退字形必须服从同一类型——`get_output_bitmap_type()` 与 `get_glyph_bitmap()`
   实际输出必须永远一致，跨字体回退尤其要强制对齐。
3. **位图度量 width/height 必须是实际位图尺寸**，不是设计 bbox——任何按行宽解码
   字形流的消费者都会因 ±1px 错位而碎。GDR 适配器是正确参照。

---

## 第 30 轮（2026-06-12，commits a757d32fb + 10e226117）：CJK 目录卡死 + 上传加固

### 错误 23：X-plore 展开含中文文件名的目录永久卡死（read_dir UTF-8/UCS-2 长度混用）

**现象** 任何带中文文件名的目录一展开，X-plore 主线程永挂 requestSema，UI 冻死。
ASCII 文件名正常。

**定位（新诊断装备）** 给 `wasm_debug_dump` 加两件套：
- `kernel_system::for_each_inflight_ipc_message`：遍历消息池打印所有
  delivered/accepted 未完成的 IPC（服务名/opcode/类型/状态/发送者）。
- wserv 焦点窗口组转储。
转储显示焦点正常在 X-plore，且无单一服务挂死——卡死在 app 自身的事件循环。
进一步用 `signal_request`/`notify_info::complete` 定向打点（按线程名过滤 X-plore），
看到主线程反复"醒来又 wait"，典型的"读到坏数据、扫描循环不终止"。

**根因** `fs_server_client::read_dir_packed`（[dirs.cpp](src/emu/services/src/fs/dirs.cpp)）
容量检查用 **UCS-2 长度**（`utf8_to_ucs2(name).length()*2`），实际写入与游标步进却用
**UTF-8 字节数**（`info->name.length()*2`）。`info->name` 是 UTF-8，每个汉字 3 字节，
UCS-2 是 2 字节/字 → 两者对 ASCII 相等（掩盖 bug），对中文不等：服务端写入步进比
客户端按描述符长度解析的步进大，**打包条目流从第一个中文名之后全部错位**，客户端
读到垃圾条目，RDir 扫描循环永不结束。

**修** 统一用 UCS-2 字节数（`utf8_to_ucs2(name).length()*2`）做容量检查、memcpy、步进。
验证：E: 展开后中文目录 + 6 个中文文件名（大小/扩展名图标全对）正确列出，无卡死。
**附带**：28 轮遗留的"X-plore CJK 文件名卡 requestSema"就是此 bug，一并解决。

**指纹**：「app 装好能跑、一碰某类数据就主线程冻死」且无单一服务挂死 = app 读到
长度/编码错位的服务端数据后自身循环不终止；打包描述符流任何"写入字节数 ≠ 客户端
按长度字段步进的字节数"都会从该点起全部错位。UTF-8/UCS-2 混用是高发区。

### 上传大文件失败排查（错误链未现，转为加固 + 诊断）

桌面 Chrome 实测 `writeFileToVFS`+`savePaths` 到 **400MB** 全链路通过（含刷新后
`syncfs(true)` 恢复 + 后续全量 reconcile，字节级校验完整），物理 VFS 读写无大文件
截断，IDBFS DB_VERSION/store 名均匹配。**核心逻辑健全，失败应为受限设备的配额/内存
峰值**。但发现上传路径两处真实缺陷并修复（[index.js](src/emu/web/pages/js/index.js)）：
1. **上传无空间预检**（设备安装路径有 `checkStorageHealth`，上传没有）→ 加
   `storage.estimate()` 预检，不足时以「需要 X / 仅剩 Y / 配额 Z」提前报错。
2. **失败级联**：savePaths 失败回退到 `EKA2L1.save()`（全量 syncfs 写整个 300MB+
   设备树）——比刚失败的定向写更重更易 OOM → 移除回退，如实报错。
3. QuotaExceededError 归类为「存储空间不足」。
dev-test.html 增 `?upltest=<MB>` / `?uplverify` 自测上传持久化与刷新恢复。
**根治大文件常驻仍需 WasmFS+OPFS 迁移**（MEMFS 在 JS 堆、IDBFS 单记录/文件的架构限制）。

### 教训

1. **打包描述符流的写入步进必须与客户端解析步进逐字节一致**——长度字段、容量检查、
   memcpy、游标四处但凡一处用了不同的长度口径（字节 vs 字符、UTF-8 vs UCS-2）就错位。
2. **诊断未 complete 的 IPC 用消息池遍历**（for_each_inflight_ipc_message）比逐服务
   加日志快得多；但"无单一服务挂死 + 主线程反复醒来"指向 app 自身循环，要转向数据正确性。
3. **复现不了的环境性失败**：与其臆测，不如把失败路径加预检 + 明确错误，把下次失败
   变成可见证据（用户原话"我没有证据"）。

---

## 第 31 轮（2026-06-12，commit 974a30133）：3D 贪吃蛇游戏中偶发抖动花屏

### 错误 24：565 检测对暗品红场景间歇误判（可逆锁的副作用）

**现象** snakes 开局前抖一下，游玩中偶发性抖一帧花屏（用户截图：暗帧上蓝色散点
网格 + 红色色块 = 棋盘场景被 565 错拆后的样子）。比较难抓。

**为什么从「永久花屏」变成「偶发抖动」** 第 28 轮把 565 转换锁改成可逆（连续 2 个
确凿 32bpp 帧解锁）。误判从此不再永久——它会"咬一口就松口"：触发 → 花 1-N 帧 →
内容变化 → 解锁恢复。这就是偶发抖动的机制。

**抓现行方法** 10 秒间隔的帧转储全是 0%（偶发帧抓不到）→ 改在分类器内部加触发
探针（cls=+1 时记录 nonzero/pair_eq/alpha_odd + 4 个匹配像素值样本），跑 120 秒
真实游戏。**804 次触发**，匹配像素全部是 **0x00100010** 类：alpha=00、R==B=0x10、
G=0 的暗品红/紫（棋盘暗部颜色）。该颜色类天然满足 hi16==lo16（XX==GG=0、RR==BB），
且不是纯灰（逃过第 28 轮的灰度排除）；暗场景 nonzero 刚过 1000 时这类像素占
14-21%，轻松超过 5% 阈值。

**修复：双指纹 AND 判定**（两层同步：[screen.cpp](src/emu/dispatch/src/screen.cpp)
DSA 层 + [graphics_driver_shared.cpp](src/emu/drivers/src/graphics/backend/graphics_driver_shared.cpp)
update_bitmap 层）：
1. `pair_eq * 20 >= nonzero`：16 位上下半相等占比 >5%（原有，565 平坦区翻倍特征）；
2. **`alpha_odd * 10 >= nonzero`：顶字节非 00/FF 的占比 >10%（新增）**。真 565 翻倍
   数据的"alpha 字节"= 565 像素高 8 位（红+绿高位，取值杂乱）；合法 X/ARGB 内容的
   alpha 恒 00 或 FF。探针证实所有误判帧 alpha_odd=0 → 该判别零误伤否决全部误判类。
附带：565 锁定/解锁日志 INFO→WARN（web 构建可见；状态切换罕见且重要）。

**验证** 同 120 秒游戏剧本：修复前 804 次触发 → 修复后 **0 次锁定事件**；真实游玩
画面（红蛇/暗棋盘/品红方块 = 正是会触发的场景类）全程干净；天地道 0 误判。

### 教训

1. **把永久锁改成可逆锁时，误判的表现形态会从「持续坏」变成「间歇坏」**——更难
   被用户描述、更难抓。可逆化必须同时把误判率降到零，否则只是把 bug 变狡猾。
2. **抓间歇性误判：把探针放进判定函数本身**（记录触发时的输入特征样本），比加密
   采样转储高效得多——804 个样本一次跑出，特征（0x00100010）直接指出判别缺口。
3. **指纹叠加的正确方向是找"合法内容不可能、目标内容必然"的正交特征**：alpha
   字节恒定性正交于 16 位翻倍性，两者 AND 后误判类（恒定 alpha 的品红帧）与命中类
   （杂乱 alpha 的真 565 帧）完全分离。

---

## 第 32 轮（2026-06-12，commit 28672e0eb）：上传文件 guest 不可见（大小写）

### 错误 25：大小写敏感宿主上的小写映射 miss——"上传成功但 X-plore 看不到"

**现象（用户描述精准）** 大文件上传到 IndexedDB 成功、刷新后宿主侧 FS.stat 正常，
但 X-plore 列目录看不到。

**定位** 新增 `wasm_ls`（经模拟器 io_system 列目录）+ dev-test `?lsdiff` 双侧对比：
JS `FS.readdir` 能看到 `Symbian-阿凡达-127.32 MB.sisx`（NFC、尺寸正确），模拟器
迭代器漏掉它。**根因不是 CJK 不是大小，是文件名里的大写字母**：
[vfs.cpp](src/emu/vfs/src/vfs.cpp) `get_real_physical_path` 在大小写敏感宿主上把
guest 路径全转小写映射物理路径（设备树约定全小写落盘），而上传前端保留原始文件名
（含大写 S/MB）写入 MEMFS → 目录迭代器 readdir 拿到名字后按虚拟路径
`get_entry_info` 重解析 → 小写路径 miss → **条目被静默丢弃**（[vfs.cpp
physical_directory::get_next_entry](src/emu/vfs/src/vfs.cpp) 的 continue），open 同理。
全小写名/纯中文名不受影响 → "小文件能看到、大文件看不到"的错觉（大游戏文件命名
惯带大写）。

**修复** ① vfs：小写映射不存在时在父目录做最终分量大小写不敏感救援（仅非 ROM 盘，
ROM 全小写免探测）→ 已持久化的旧混合大小写文件立即恢复可见可读；② 前端上传
`toLowerCase` 文件名。验证：模拟器 [ls] 与 X-plore 均列出该文件且排序正确；
管家/X-plore 冒烟无回归。

### 教训

1. **"A 侧有、B 侧没有"的失踪文件，先做双侧 listing diff**（宿主 FS vs guest io），
   一条命令分离问题域——wasm_ls + ?lsdiff 现已入永久装备。
2. **静默 continue 丢弃迭代条目是隐形错误放大器**：get_entry_info 失败的目录条目
   直接消失，没有任何日志。同类模式（解析失败→跳过）值得全库审视。
3. **跨边界写入必须遵守对侧的命名约定**：JS 侧写 MEMFS 绕过了模拟器的"全小写"
   约定，两个世界各自正确、组合即 bug。
