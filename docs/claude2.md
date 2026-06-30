# EKA2L1 WASM Web 版调试记录（第三轮）

> 对话时间：2026-05-14
> 接续 [claude.md](./claude.md)，继续修复持久化、启动崩溃、C++ 异常问题
> 目标：刷新后设备列表正常恢复、app 列表保持

---

## 错误链与修复过程

### 错误 19：打开页面立刻 `memory access out of bounds` in `spdlog::logger::log_it_`

**现象**
刷新页面，splash 还没消失就报：
```
Uncaught RuntimeError: memory access out of bounds
  at spdlog::logger::log_it_(...)
  ← wasm_init_with_rom (postRun → ccall)
```

**根因**
`moduleConfig.noInitialRun = true` 让 Emscripten 不自动调 `main()`。但旧代码在 `postRun` 的 setTimeout 里直接调了 `wasm_init_with_rom("", "")`——此时 `main()` 还没跑过，`eka2l1::log::spd_logger` 仍为空 `shared_ptr`。`LOG_INFO` 宏在非 ENABLE_SCRIPTING 路径下只检查 `filterings->is_passed()`，不检查 `filterings` / `spd_logger` 是否为 null，解引用 null → WASM trap。

**修复**（两处）

1. `src/emu/web/shell.html`：把 postRun 简化为只更新进度条；设备恢复调用 `wasm_init_with_rom("","")` 挪到 `startMain()` 里 `callMain([])` 之后（main 已跑、logger 已就绪）。`callMain` 因 `simulate_infinite_loop=1` 会抛 `unwind` 异常，需 try/catch 掉以让代码继续：
```js
try { mod.callMain([]); }
catch (e) {
    if (!(e === 'unwind' || e.name === 'ExitStatus'))
        console.error('main() threw unexpected error:', e);
}
// 此时 spd_logger 已就绪，可安全调用
var result = module.ccall('wasm_init_with_rom', 'number', ['string','string'], ['','']);
```

2. `src/emu/common/include/common/log.h`：`COND_CHECK` 宏（无论是否 ENABLE_SCRIPTING）统一加 `spd_logger &&` 和 `filterings &&` 的 null-check，彻底避免任何 LOG_* 在 logger 未就绪时崩溃：
```cpp
// 改前（非 ENABLE_SCRIPTING）：
#define COND_CHECK(class, serv) if (eka2l1::log::filterings->is_passed(class, spdlog::level::serv))
// 改后：
#define COND_CHECK(class, serv) if (eka2l1::log::spd_logger && eka2l1::log::filterings && eka2l1::log::filterings->is_passed(class, spdlog::level::serv))
```

3. `src/emu/web/src/main.cpp`：`wasm_init_with_rom` 入口加防御：
```cpp
if (!eka2l1::log::spd_logger) {
    eka2l1::log::setup_log(nullptr);
    eka2l1::log::toggle_console();
}
```

---

### 错误 20：浏览器缓存旧版 `eka2l1.js` 导致新代码不生效

**现象**
修改了 shell.html 并重新构建，但控制台日志仍显示旧代码路径（`eka2l1.html:699:45`），新加的 log 语句不出现。

**根因**
`python3 serve.py` 基于 `SimpleHTTPRequestHandler`，默认发送 `Last-Modified` 响应头。浏览器对 `eka2l1.js`（7MB 大文件）做了强缓存，`Cmd+Shift+R` 只刷新 html 本身，js 文件仍用缓存版。旧版 js 没有 `createEKA2L1Module` 导出，走了 fallback 路径，IDBFS 从未被 mount，所有持久化操作静默跳过。

**修复**（`src/emu/web/serve.py`）
加 `Cache-Control: no-store` 头，**需重启服务器**：
```python
def end_headers(self):
    self.send_header("Cross-Origin-Opener-Policy", "same-origin")
    self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
    self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
    self.send_header("Pragma", "no-cache")
    super().end_headers()
```

---

### 错误 21：IDBFS `syncfs` 报 `InvalidStateError: The database connection is closing`

**现象**
ROM 安装成功后调 `syncfs(false)`，报：
```
InvalidStateError: Failed to execute 'transaction' on 'IDBDatabase':
The database connection is closing.
  at Object.reconcile (eka2l1.js:2859)
  at index.openKeyCursor.onsuccess (eka2l1.js:2726)
```

**根因**
Emscripten IDBFS 在 `syncfs` 内部缓存一个 IDBDatabase handle（`IDBFS.dbs[name]`），并在 `getRemoteSet`（readonly tx）和 `reconcile`（readwrite tx）两步中**复用**同一个 handle。在 wasm 长任务（ROM 安装）结束后，浏览器把这个长期持有的 db handle 标记为 "closing"，下一个 transaction 就抛 `InvalidStateError`。

这个 bug 同时影响 `syncfs(false)`（保存）和 `syncfs(true)`（加载）两条路径——刷新后加载也会失败，数据永远读不回来。

**修复**（`src/emu/web/shell.html`）
Monkey-patch `IDBFS.getDB`，使其**每次都打开新连接**，不使用缓存：
```js
(function patchIDBFS() {
    var IDBFS = mod.FS.filesystems && mod.FS.filesystems.IDBFS;
    if (!IDBFS) { console.warn('[EKA2L1] IDBFS not found'); return; }
    IDBFS.getDB = function(name, callback) {
        // 永远不缓存，每次都新开连接
        var req = IDBFS.indexedDB().open(name, IDBFS.DB_VERSION);
        req.onupgradeneeded = function(e) { /* 建库/建 store 逻辑不变 */ };
        req.onsuccess = function() {
            var db = req.result;
            db.onversionchange = function() { db.close(); };
            callback(null, db);
        };
        req.onerror = function(e) { callback(e.target.error); e.preventDefault(); };
    };
    IDBFS.dbs = {}; // 清掉旧缓存
    console.log('[EKA2L1] IDBFS.getDB patched (no-cache mode)');
})();
```

patch 在 `createEKA2L1Module(...).then(mod => ...)` 回调里、mount IDBFS 之前执行。

---

### 错误 22：`devices.yml` 从未写到磁盘，刷新后设备列表丢失

**现象**
IDBFS 正常工作后，刷新页面仍然 `device_manager loaded 0 device(s)`，IDBFS 里有 `drives/`、`roms/` 等目录但**没有 `devices.yml`**。

**根因**
`loader::install_rpkg()` 和 `loader::install_rom()` 内部调 `dvcmngr->add_new_device()`，把设备加到内存列表，但**从不调 `save_devices()`**。`save_devices()` 只在以下场景被调用：
- `load_devices()` 末尾（反序列化后重新序列化）
- `delete_device()`
- `system_impl` 的 `install_device()` 路径（桌面版路径，WASM 不走）

所以 `devices.yml` 永远不会被写入磁盘。每次刷新 `device_manager` 构造时读不到文件，设备为 0。

**修复**（`src/emu/web/src/main.cpp`，`wasm_init_with_rom` 安装完成后）
```cpp
// install_rpkg/install_rom 调 add_new_device() 但不调 save_devices()，
// 不显式调用的话 devices.yml 永远不写盘，刷新后设备丢失。
dvcmngr->save_devices();
LOG_INFO(FRONTEND_CMDLINE, "{} device(s) now registered, devices.yml saved", dvcmngr->total());
```

---

### 错误 23：`YAML::ScanScalar` 抛 C++ 异常导致 Aborted

**现象**
持久化修好后，刷新加载 `devices.yml` 时崩溃：
```
Aborted(Assertion failed: Exception thrown, but exception catching is not enabled.
  Compile with -sNO_DISABLE_EXCEPTION_CATCHING or -sEXCEPTION_CATCHING_ALLOWED=[..])
  at YAML::ScanScalar → YAML::Scanner::ScanNextToken → YAML::SingleDocParser::HandleMap
```

**根因**
yaml-cpp 内部在解析 YAML 时会抛 C++ 异常（`YAML::ParserException` 等）。Emscripten 默认**禁用 C++ 异常捕获**（`-fno-exceptions`），所有 throw 都直接 abort，`try/catch` 块不起作用。`load_devices()` 里虽然有 `try { YAML::Load(...) } catch (YAML::Exception e) { return; }`，但在 WASM 下 catch 永远捕获不到，直接崩溃。

**修复**（`src/emu/web/CMakeLists.txt`）
启用异常捕获，需同时加编译和链接标志，并 `--clean-first` 完整重编所有依赖库（yaml-cpp 等）：
```cmake
# yaml-cpp 等库内部抛 C++ 异常，必须启用异常捕获
add_compile_options(-fexceptions)
target_compile_options(eka2l1_wasm PRIVATE -fexceptions)
# 链接时告诉 Emscripten 不要禁用异常
target_link_options(eka2l1_wasm PRIVATE
    "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
    ...
)
```

构建时必须 `--clean-first` 保证 yaml-cpp 等库也用新标志重编：
```bash
cmake --build build_wasm_test --target eka2l1_wasm -j4 --clean-first
```

---

## 涉及文件汇总（本轮新增/修改）

| 文件 | 改动 |
|---|---|
| `src/emu/common/include/common/log.h` | `COND_CHECK` 宏统一加 `spd_logger &&` 和 `filterings &&` null-check |
| `src/emu/web/src/main.cpp` | `wasm_init_with_rom` 入口加 logger null-check；安装后显式调 `dvcmngr->save_devices()` |
| `src/emu/web/shell.html` | postRun 只更新进度；设备恢复挪到 callMain 之后；patch `IDBFS.getDB` 为 no-cache 模式；syncfs(false) 加日志 |
| `src/emu/web/serve.py` | 加 `Cache-Control: no-store` 头，避免浏览器缓存旧 JS |
| `src/emu/web/CMakeLists.txt` | 加 `-fexceptions` 和 `-s DISABLE_EXCEPTION_CATCHING=0`，启用 C++ 异常捕获 |

---

## 关键经验（本轮新增）

6. **`noInitialRun: true` + postRun 陷阱**：postRun 在 `callMain()` 之前触发，此时 C++ 全局状态（spdlog logger 等）尚未初始化。任何在 postRun 里调用的 C++ 导出函数都可能因 logger 未就绪而崩溃。正确做法：在 `callMain()` 之后再调。

7. **Emscripten IDBFS 连接缓存 bug**：`IDBFS.getDB` 缓存 IDBDatabase handle，在长 wasm 任务后 handle 会进入 "closing" 状态。下次 `syncfs` 开 transaction 时报 `InvalidStateError`。修复方法：monkey-patch `getDB`，每次都 `indexedDB.open()` 新连接。

8. **`install_rpkg`/`install_rom` 不调 `save_devices()`**：这是 EKA2L1 原有 bug（桌面版用 `system_impl::install_device()` 路径，会调 `save_devices()`；但 WASM 版直接调 loader 函数，绕过了这一步）。WASM 版必须在安装后手动调 `dvcmngr->save_devices()`。

9. **`-fexceptions` 必须全量重编**：yaml-cpp 等依赖库在没有 `-fexceptions` 时编译，其内部的 throw 指令仍会 abort。只给 `eka2l1_wasm` 目标加标志不够，必须用 `add_compile_options(-fexceptions)` + `--clean-first` 全量重编。

10. **浏览器缓存大文件**：`Cmd+Shift+R` 只强制刷新 HTML，7MB 的 `eka2l1.js` 可能被强缓存。serve.py 必须加 `Cache-Control: no-store`，且**重启服务器**才生效。

---

## 构建命令

```bash
# 首次或改了 CMakeLists.txt 时（全量重编）
cmake --build build_wasm_test --target eka2l1_wasm -j4 --clean-first

# 只改了 main.cpp / shell.html 时（增量）
cmake --build build_wasm_test --target eka2l1_wasm -j4

# 测试（需重启才能让 Cache-Control 生效）
cd build_wasm_test/bin
python3 ../../src/emu/web/serve.py 8080 .
```

---

## 当前状态

- [x] 启动崩溃（spdlog null-deref）已修复
- [x] IDBFS connection closing 错误已修复（patch IDBFS.getDB）
- [x] devices.yml 未写盘问题已修复（安装后显式调 save_devices）
- [x] C++ 异常（yaml-cpp ScanScalar abort）已修复（启用 -fexceptions）
- [ ] 刷新后 app 列表是否恢复（等用户验证）
- [ ] 软件打开后黑屏问题（待分析）
