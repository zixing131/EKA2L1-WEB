# EKA2L1 WASM Web 版调试记录（续 3）

> 对话时间：2026-05-14 ~ 2026-05-15
> 接续 [kilo.md](./kilo.md) / [CLAUDE.md](./CLAUDE.md)
> 目标：定位"启动 app 后页面卡死、按钮无法点击"的真正根因

---

## 起点状态

接手时已知：
- IDBFS / 设备恢复 / app 列表加载 / dyncom watchdog / 8ms frame budget 等前期修复都在
- 仍然现象：点击 calculator app → 启动一段时间后浏览器页面"卡死"，HTML 按钮无响应
- 之前认为可能是 dyncom 死循环（kilo.md 里加了 watchdog），实际 watchdog 几乎没触发

---

## 调试方法

诊断主要靠：
1. **DevTools Performance trace** 多次抓取
2. **逐步加细的心跳/计时日志**（避免被 spdlog 自身淹没）
3. **Edge 浏览器任务管理器**（看 tab 真实 CPU% / 内存）

---

## 错误链与修复

### 错误 20：误诊 1 —— `c:\resource\errrd not found` 是卡死原因

**现象**
日志反复停在 `Get entry of: c:\resource\errrd`。

**实际**
errrd 已经正常 `not found` 返回。Symbian 资源加载会按 `.r01 → .r → .rsc` 顺序探测，前面 not found 是**正常 fallback 链**，errrd 不存在更是预期（避免进入错误报告模式）。

**结论**
不是问题。日志停在 errrd 后是因为后面的 syscall 真的就停了。

---

### 错误 21：SDL audio 在主线程做实时重采样

**诊断**
DevTools Performance trace 显示：

| 采样数 | 函数 |
|---|---|
| 580312 | `callUserCallback` |
| **48431** | **`SDL_ResampleAudio`** |
| 48401 | `js-to-wasm:ii:i` |
| 8364 | `InterpreterTranslateInstruction` |
| 2127 | `InterpreterMainLoop` |

调用链：
```
SDL2.audio.scriptProcessorNode.onaudioprocess  ← 浏览器主线程
  → HandleAudioProcess
    → SDL_AudioStreamPut → SDL_ResampleAudioStream → SDL_ResampleAudio
```

**根因**
SDL2 在 Emscripten 下用 deprecated `ScriptProcessorNode` 实现音频，回调在**浏览器主线程**同步执行 `SDL_ResampleAudio` + `SDL_Convert_S16_to_F32_Scalar`。每次回调都吃大量主线程时间，挤占 RAF。

**修复**（[src/emu/web/src/main.cpp](src/emu/web/src/main.cpp)）

`init_sdl()` 中：
```cpp
#ifdef __EMSCRIPTEN__
    constexpr Uint32 SDL_INIT_FLAGS = SDL_INIT_VIDEO | SDL_INIT_TIMER;
#else
    constexpr Uint32 SDL_INIT_FLAGS = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
#endif
```

`SDL_OpenAudioDevice` 整段也用 `#ifndef __EMSCRIPTEN__` 包起来。

**结果**
新 trace 里 `SDL_ResampleAudio` 从 48431 降到 39。音频路径完全不再消耗主线程。

---

### 错误 22：误诊 2 —— "卡死"实际上是 spdlog 把 DevTools Console 淹了

**现象**
为定位卡死位置，给 `main_loop` / `system_impl::loop` 加了密集心跳（每 frame、每 slice 4 行 LOG_INFO）。
跑起来后控制台日志疯狂滚动，浏览器看起来"卡死"。

**关键证据**
心跳显示 `frame=1009 → 1010 → ...` 一直推进，**main_loop 完全没卡**。
之前 trace `wasm-to-js` 占 432615 个采样的大头 —— 几乎全是 spdlog → console.log。
主线程最大 stall 只有 17ms，**根本没有死锁**。

**根因**
WASM 下每条 spdlog 都走 `wasm-to-js → console.log`（同步 + 跨语言边界）。日志多到每秒上千条时，DevTools console 的 DOM 渲染队列爆炸，整个 Chrome 渲染进程被拖死。这导致：
- DevTools Performance 录制都没法保存
- 用户判定为"页面卡死"

**修复**（[src/emu/common/src/log.cpp](src/emu/common/src/log.cpp)）

WASM 下默认日志级别提到 `warn`：
```cpp
#ifdef __EMSCRIPTEN__
    spdlog::set_level(spdlog::level::warn);
    spd_logger->flush_on(spdlog::level::err);
#else
    spdlog::set_level(spdlog::level::trace);
    spd_logger->flush_on(spdlog::level::debug);
#endif
    filterings = std::make_unique<log_filterings>();
#ifdef __EMSCRIPTEN__
    filterings->reset_all(spdlog::level::warn);
#endif
```

**结果**
控制台只剩 WARN/ERR，DevTools 不再被淹。`fs.cpp` 的 INFO 探针自动失效。

---

### 错误 23：HTML 按钮无响应 + Canvas 黑屏 + Edge 任务管理器 CPU 100%

**诊断**
低频心跳（每 2 秒一次）显示：
```
frames_since=120  ~  121     ← 60FPS 完美
heap=512MB  used~373MB        ← 完全稳定
delta=0MB                     ← 没有泄漏
```

但 Edge 任务管理器：
- **CPU 99.7%**
- 内存 974 MB（之前 ~512MB → 增长到 974MB）
- 弹出"此页面没有响应"对话框
- 标签标题仍 "60 FPS"

**说明**
- ✅ 主线程没死锁（RAF 60FPS）
- ✅ 没 OOM（heap 稳定）
- ✅ dyncom 不卡（watchdog 仅 1 次）
- ❌ **wasm 把整个 CPU 核占满 → 浏览器主线程没空处理 DOM 事件 → click 派发不出去**
- ❌ 内存涨是 wasm grow_memory（不是泄漏）

**第一次缓解尝试**
[src/emu/web/src/main.cpp](src/emu/web/src/main.cpp) main_loop budget 收紧：
```cpp
// 从 8.0 / 64 降到 2.0 / 8
static constexpr double FRAME_CPU_BUDGET_MS = 2.0;
static constexpr int MAX_SLICES_PER_FRAME = 8;
```

但收紧后 CPU 仍 100% —— 说明**单次 `loop()` 调用本身就慢**，不是 budget 多设几次的问题。

---

### 错误 24：dyncom `instruction_cache` unordered_map rehash 把 CPU 吃光

**诊断**
另一次 Performance trace 的 sample 排名（启用 warn-level 后）：

| 采样数 | 函数 |
|---|---|
| **456542** | **`unordered_map<uint32_t, size_t>::__emplace_unique_key_args`** |
| 38002 | `operator new(unsigned long)` |
| 32765 | `(idle)` |
| 3149 | `InterpreterTranslateInstruction` |
| 805 | `InterpreterMainLoop` |
| 19 | `dyncom_core::run` |

调用链：
```
main_loop → dyncom_core::run → InterpreterMainLoop
  → InterpreterTranslate{Block,Single}
    → cpu->instruction_cache[pc_start] = bb_start
      → unordered_map::__emplace_unique_key_args  ← 90%+ CPU
        → operator new                              ← hash node 分配
```

`emplace` 占采样数比 `InterpreterMainLoop` 大 **567 倍** —— 不是调用次数 567 倍，是**单次 emplace 时间**远大于其他代码（hash table rehash + new + 内存分配/释放）。

**根因**
- [src/emu/cpu/include/cpu/dyncom/armstate.h:246](src/emu/cpu/include/cpu/dyncom/armstate.h#L246):
  ```cpp
  std::unordered_map<std::uint32_t, std::size_t> instruction_cache;
  ```
- guest 持续走新 PC → 不断 `emplace` 新 entry
- map 默认初始 bucket 数极小，**反复触发 rehash**（每次 rehash 都重新分配 bucket 数组 + 搬所有节点）
- map 永不 shrink，guest 跑越久 hash 越大、rehash 越慢
- 在 WASM 里：每个 `operator new` / `delete` 都是相对慢的调用 + emscripten 的 dlmalloc 在多线程下加锁 → 主线程被 hash table 操作锁满

**修复 1**：预分配，避开初期反复 rehash
[src/emu/cpu/src/dyncom/armstate.cpp](src/emu/cpu/src/dyncom/armstate.cpp)
```cpp
ARMul_State::ARMul_State(...) : core(core) {
    instruction_cache.reserve(64 * 1024);
    Reset();
    ChangePrivilegeMode(initial_mode);
}
```

**修复 2**：超过阈值就清空缓存 + 重置翻译 buffer
[src/emu/cpu/src/dyncom/arm_dyncom_interpreter.cpp](src/emu/cpu/src/dyncom/arm_dyncom_interpreter.cpp)
```cpp
static constexpr std::size_t INSTRUCTION_CACHE_FLUSH_THRESHOLD = 256 * 1024;

static inline void maybe_flush_instruction_cache(ARMul_State *cpu) {
    if (cpu->instruction_cache.size() >= INSTRUCTION_CACHE_FLUSH_THRESHOLD) {
        cpu->instruction_cache.clear();
        cpu->trans_cache_buf_top = 0;
    }
}
```
在 `InterpreterTranslateBlock` 和 `InterpreterTranslateSingle` 入口调用。

**结果**
- heap delta 仍 0MB（修复有效，hash 不再无界增长）
- 但 **CPU 仍 100%**，HTML 按钮仍迟钝
- 说明 hash table 病修了，**还有第二个性能热点**

---

## 当前未解决：第二个 100% CPU 热点

修了 instruction_cache 后状态：
```
frames_since=120  ~  121     ← 60FPS 仍稳
heap=512MB  used~373MB        ← 稳定
delta=0MB ... 偶尔 5MB        ← 几乎不增长
```

但 CPU 仍 100% + 页面响应迟钝（要点两次按钮才能关闭）。

为定位"每帧到底什么吃 CPU"，加了 per-frame 计时心跳：

[src/emu/web/src/main.cpp](src/emu/web/src/main.cpp) 改成：
```
[hb] t=... frames=N heap=...MB used~...MB d=...MB
    | per-frame avg loop=X.XXms swap=Y.YYms total=Z.ZZms
    | max loop=A.AAms swap=B.BBms total=C.CCms
```

测量 3 段：
- `loop` = `system_impl::loop()` 累计耗时
- `swap` = `SDL_GL_SwapWindow` 耗时
- `total` = main_loop 总耗时

构建过程中**main_loop 函数头被误删过一次**（用 Edit 替换大段时把 `static void main_loop() {` 吃掉了），后来手动补回。

**下一步**：等用户跑这版构建，根据 avg/max 数字判断：
- `swap` 占大头 → mac WebGL2 swap 慢，需要换 OffscreenCanvas / 减少 swap
- `loop` 占大头 → wasm cpu 路径还有热点，profile 找
- 都不大但 total 大 → 心跳 EM_ASM 本身或 worker pthread 在抢

---

## 关键经验

1. **不要相信 spdlog "trace" / "info" 在 WASM 下的成本**：每条都同步跨 wasm-to-js 边界 + console.log，DevTools console 渲染会成为整个浏览器进程的瓶颈，**伪装成"主线程死锁"**。WASM 下默认 warn 级别。

2. **诊断前先排除"DevTools 自身慢"**：抓 trace 时关掉 DevTools 多余面板（特别是 Network、Memory）；用浏览器**任务管理器**看 tab 真实 CPU%，比看 DevTools 准。

3. **`unordered_map` 在热路径上要 reserve**：默认极小 bucket 会触发反复 rehash。即使总 sbrk 看起来稳定，rehash 也会在 dlmalloc 内部反复 alloc/free，吃光 CPU。

4. **"卡死" ≠ "死锁"**：用低频心跳（2 秒打 1 行 WARN）+ frame counter 区分：
   - 心跳停 → 真死锁
   - 心跳持续但每次 frames_since=1 → RAF 节流（标签后台/慢）
   - 心跳持续且 frames_since≈120 → 主线程在跑，"卡死"是 DOM 事件来不及处理

5. **Performance trace 的样本计数 ≠ 调用次数**：占 90% 样本的函数代表 90% **CPU 时间**，可能是**少数次调用但每次很慢**，也可能是**大量快速调用**。要看父子关系才能区分。

6. **SDL2 + Emscripten 默认配置在 mac 上特别糟**：ScriptProcessorNode 在主线程跑音频；某些路径 `SDL_PollEvent → SDL_WaitEventTimeout` 也会拖慢主线程。

---

## 涉及文件汇总（本轮新增/修改）

| 文件 | 改动 |
|---|---|
| `src/emu/web/src/main.cpp` | WASM 下禁用 SDL_INIT_AUDIO 和 SDL_OpenAudioDevice；frame budget 从 8ms/64 降到 2ms/8；main_loop 加 low-freq heartbeat（heap+used+delta+per-frame loop/swap/total ms） |
| `src/emu/common/src/log.cpp` | WASM 下默认日志级别 = warn；filterings reset 到 warn |
| `src/emu/cpu/src/dyncom/armstate.cpp` | `instruction_cache.reserve(64 * 1024)` |
| `src/emu/cpu/src/dyncom/arm_dyncom_interpreter.cpp` | 加 `maybe_flush_instruction_cache`，超过 256K entry 就 clear 缓存和翻译 buffer；watchdog log 改 LOG_WARN（穿透 warn 过滤器）+ 指数退避去重 |
| `src/emu/system/src/epoc.cpp` | 调试期临时心跳，已删除（保留原有 Slow WASM CPU slice 警告） |

---

## 当前下一步

1. 用 per-frame 计时构建测试，根据 `avg loop/swap/total` 数字定位第二个 CPU 热点：
   - 如果是 `swap` —— 优化 WebGL 上传 / 改用更小 canvas / 降帧率到 30FPS
   - 如果是 `loop` —— 进一步 profile dyncom 内部，找下一个热点
   - 如果都不大 —— 调查 worker pthread 是否在抢 CPU（PTHREAD_POOL_SIZE=32 可能过大）

2. 解决"calculator 黑屏"是另一类问题：
   - `Object handle is invalid 2596096` / `0` —— window server handle 异常
   - `Unable to find parent for new group with ID = 0x0. Use root` —— window group 父子关系不对
   - `Getting packed struct with mismatch size (72 vs 64)` —— IPC packed struct ABI 错配
   - 这些跟"卡死"无关，独立排查

3. 考虑是否要换浏览器测试（mac Chrome 而不是 Edge），或用别的设备验证 SwapWindow 是否慢。
