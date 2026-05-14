# EKA2L1 WASM Web 版调试记录（Kilo）

> 对话时间：2026-05-14
> 接续 `claude2.md`，继续修复刷新后 app 列表、WASM app 启动黑屏/卡死、浏览器主线程长时间阻塞问题。

---

## 当前调试目标

- 刷新后设备列表和 app 列表能恢复。
- 点击 app 后不再让浏览器页面卡死。
- 继续定位 app 启动后黑屏/无画面问题。

---

## 已确认状态

- IDBFS 已能恢复 `/eka2l1` 内容。
- `devices.yml` 已能从 IndexedDB 恢复。
- 刷新后 app 列表已能加载。
- 点击 app 后能创建 Symbian 进程，并进入 Avkon/ECom/CDL/Icon server 初始化阶段。
- 之前看似卡在 `c:\resource\errrd`，实际 `RFs::Entry` 已正常返回 `not found`，卡死发生在 guest CPU 继续执行之后。

---

## 问题 1：刷新后 app 列表仍丢失

### 现象

刷新后日志显示：

```text
[EKA2L1] /eka2l1 after restore:
['devices.yml', 'drives', 'j2me', 'rom_upload.rom', 'roms', 'rpkg', 'rpkg_upload.rpkg', 'temp']
[EKA2L1] devices.yml size=182
Failed to parse devices.yml: yaml-cpp: error at line 6, column 19: unknown escape character: D
device_manager loaded 0 device(s) from /eka2l1
ROM file not found or not readable:
```

### 根因

IDBFS 恢复是成功的，但 `devices.yml` 内容本身是非法 YAML。某个设备字段里包含类似 `\D` 的反斜杠序列，被写进 YAML 双引号字符串后触发 yaml-cpp 错误：

```text
unknown escape character: D
```

`device_manager` 加载失败后设备数为 0，`wasm_init_with_rom("", "")` 无法恢复设备，也没有 ROM 路径可重新安装，所以 app 列表上下文丢失。

### 修复

文件：`src/emu/system/src/devices.cpp`

1. `load_devices()` 增加旧坏文件恢复路径：
   - 先正常用 yaml-cpp 解析。
   - 失败后尝试修复双引号字符串里的非法 escape。
   - 仍失败则用 permissive parser 按 `devices.yml` 简单结构宽松扫描恢复 device。

2. `save_devices()` 改成用 YAML 单引号标量保存关键字段，避免后续再写出非法 YAML：
   - `firmware_code`
   - `platver`
   - `manufacturer`
   - `firmcode`
   - `model`

### 结果

刷新后设备和 app 列表可恢复。旧坏 `devices.yml` 第一次被 permissive parser 读入后，会在 `load_devices()` 末尾重新 `save_devices()` 成安全格式。

---

## 问题 2：点击 app 后黑屏/卡住，最初疑似停在 `c:\resource\errrd`

### 现象

日志最初反复显示资源加载，最后停在：

```text
Get entry of: c:\resource\errrd
```

浏览器页面不可点击，像是卡死。

### 初步排查

给 `src/emu/services/src/fs/fs.cpp` 的 `fs_server_client::entry()` 增加探针：

- `Slow get_entry_info ...`
- `Get entry result ... found/not found`
- `Get entry completed ok/not found`

后续日志确认：

```text
Get entry of: c:\resource\errrd
Get entry result for c:\resource\errrd: not found
Get entry completed not found: c:\resource\errrd
```

因此 `errrd` 文件查询不是卡死点。FS/VFS 已正常返回。

---

## 问题 3：Web app launch 行为与桌面 Qt 不一致

### 现象

Web app 启动代码使用：

```cpp
cmdline.launch_cmd_ = epoc::apa::command_create;
```

桌面 Qt 从 app list 启动使用的是 `command_open`。

### 修复

文件：`src/emu/web/src/main.cpp`

将 Web app-list 启动命令改成：

```cpp
cmdline.launch_cmd_ = epoc::apa::command_open;
```

并保留/新增：

- `wasm_launch_app()` 成功后设置 `g_state.paused = false`。
- 给 `alserv->launch_app()` 外层加 C++ 异常捕获。
- app 退出时记录：

```text
App process exited: name=... uid=... exit_type=... category=... reason=...
```

文件：`src/emu/web/shell.html`

点击 app 且 `wasm_launch_app()` 返回 0 后调用：

```js
startEmulation();
```

避免 app 已启动但 emulator 仍处于 paused。

---

## 问题 4：浏览器主线程被 guest CPU 长时间占住

### 现象

点击 app 后页面可能不可点击，控制台不继续输出，表现为浏览器主线程卡死。

### 根因判断

`system_impl::loop()` 每次浏览器 RAF 回调只调用一次：

```cpp
cpu->run(to_run->get_remaining_screenticks());
```

完整 Symbian timeslice 在 WASM/dyncom 下可能跑太久，导致 JS 事件循环无法返回。

### 第一阶段修复：限制单次 CPU ticks

文件：`src/emu/system/src/epoc.cpp`

WASM 下限制单次 `cpu->run()` ticks：

```cpp
#ifdef __EMSCRIPTEN__
static constexpr int MAX_WASM_TICKS_PER_LOOP = 20000;
ticks_to_run = std::min(ticks_to_run, MAX_WASM_TICKS_PER_LOOP);
#endif
```

并加耗时探针：

```cpp
Slow WASM CPU slice: ... ticks took ...us
```

### 第二阶段修复：Web main loop 使用 wall-clock budget

文件：`src/emu/web/src/main.cpp`

原来每个 browser frame 只跑一次 `symsys->loop()`，降 ticks 后启动推进很慢。改成每帧最多跑 8ms 的多个短 slice：

```cpp
static constexpr double FRAME_CPU_BUDGET_MS = 8.0;
static constexpr int MAX_SLICES_PER_FRAME = 64;

const double frame_start = emscripten_get_now();
int slices = 0;
while (slices < MAX_SLICES_PER_FRAME) {
    const int loop_result = g_state.symsys->loop();
    ++slices;

    if (loop_result == 0) {
        break;
    }

    if ((emscripten_get_now() - frame_start) >= FRAME_CPU_BUDGET_MS) {
        break;
    }
}
```

每 5 秒输出一次状态：

```text
WASM frame: slices=... budget_ms=... process=... thread=... state=...
```

### 结果

日志显示 app 启动能推进到：

- `Symbian3rd` 进程创建。
- `AknIconSrv` 创建。
- `ecomserver` 创建。
- `CdlServer` 创建。
- 资源文件、插件目录扫描、CDL 资源加载。

示例：

```text
WASM frame: slices=14 budget_ms=8.32 process=ecomserver[10009d8f]0001 thread=!ecomserver state=1
WASM frame: slices=13 budget_ms=8.40 process=ecomserver[10009d8f]0001 thread=!ecomserver state=1
```

---

## 问题 5：FS 返回后仍卡死，dyncom 解释器内部不及时 yield

### 现象

完整 `log.txt` 最终仍停在：

```text
Get entry of: c:\resource\errrd
Get entry result for c:\resource\errrd: not found
Get entry completed not found: c:\resource\errrd
```

由于 `fs.cpp` 的 complete 日志已经出现，说明卡死发生在返回 guest 代码之后。外层 `epoc.cpp` 的 `cpu->run()` 后续日志没有机会打印，说明 dyncom 解释器可能进入后长时间不返回。

### 修复

文件：`src/emu/cpu/src/dyncom/arm_dyncom_interpreter.cpp`

新增 WASM 专用 wall-clock watchdog：

```cpp
#ifdef __EMSCRIPTEN__
static constexpr std::uint64_t WASM_DYNCOM_RUN_BUDGET_US = 8000;
const std::uint64_t wasm_dyncom_deadline_us = eka2l1::common::get_current_utc_time_in_microseconds_since_epoch()
    + WASM_DYNCOM_RUN_BUDGET_US;
bool wasm_dyncom_deadline_hit = false;
#endif
```

新增宏，每 1024 条 guest 指令检查一次时间：

```cpp
#ifdef __EMSCRIPTEN__
#define WASM_DYNCOM_YIELD_CHECK                                                \
    if (((num_instrs & 0x3FF) == 0)                                             \
        && (eka2l1::common::get_current_utc_time_in_microseconds_since_epoch()  \
            >= wasm_dyncom_deadline_us)) {                                      \
        wasm_dyncom_deadline_hit = true;                                        \
        goto END;                                                               \
    }
#else
#define WASM_DYNCOM_YIELD_CHECK
#endif
```

插入到 `GOTO_NEXT_INST` 中：

```cpp
#define GOTO_NEXT_INST                         \
    if (num_instrs >= cpu->NumInstrsToExecute) \
        goto END;                              \
    num_instrs++;                              \
    WASM_DYNCOM_YIELD_CHECK;                   \
    goto *InstLabel[inst_base->idx]
```

watchdog 命中时输出：

```text
WASM dyncom yielded after ... instructions
```

### 预期

这应避免 guest dyncom 解释循环长期占住浏览器主线程。若之后页面不再卡死但仍黑屏，则下一阶段问题将从“主线程阻塞”转为“guest/app 等待、窗口服务或画面 present/红raw 链路未正常工作”。

---

## 构建验证

多次执行：

```bash
cmake --build build_wasm_test --target eka2l1_wasm -j4
```

均构建成功。出现的 warning 为已有 warning：

- `epoc.cpp` 旧的 `-Wlogical-not-parentheses`。
- 触摸压力 float conversion warning。
- Emscripten `-pthread + ALLOW_MEMORY_GROWTH` warning。

---

## 当前涉及文件

| 文件 | 改动 |
|---|---|
| `src/emu/system/src/devices.cpp` | 宽松恢复坏 `devices.yml`；保存设备字段时使用单引号 YAML 标量 |
| `src/emu/web/src/main.cpp` | app launch 改 `command_open`；launch 异常/退出日志；主循环 8ms frame budget；状态日志 |
| `src/emu/web/shell.html` | 点击 app 成功后调用 `startEmulation()` |
| `src/emu/system/src/epoc.cpp` | WASM CPU slice ticks 限制到 20000；CPU slice 慢日志 |
| `src/emu/services/src/fs/fs.cpp` | `RFs::Entry` 完成/慢查询探针 |
| `src/emu/cpu/src/dyncom/arm_dyncom_interpreter.cpp` | WASM dyncom 内部 8ms watchdog，每 1024 指令 yield |

---

## 当前下一步

1. 用包含 dyncom watchdog 的新构建重新测试启动 app。
2. 观察是否出现：

```text
WASM dyncom yielded after ... instructions
```

3. 如果页面不再卡死但仍黑屏，继续定位：
   - window server 是否有 foreground window group。
   - screen buffer 是否有更新。
   - OpenGL/SDL swap 是否 present 到 canvas。
   - app 是否在 wait 状态等待某个 service/event。

4. 如果仍卡死且没有 `WASM dyncom yielded` 日志，则继续深入 dyncom 单条指令 handler 或 syscall 回调路径，查是否有非 `GOTO_NEXT_INST` 的内部死循环。
