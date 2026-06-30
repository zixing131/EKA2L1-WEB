# EKA2L1 WASM Web 版调试记录

> 对话时间：2026-05-13  
> 目标：修复 EKA2L1 塞班模拟器 Web/WASM 版本的 ROM/RPKG 安装崩溃问题

---

## 问题链与修复过程

### 错误 1：`null function` at `eka2l1::io_system::exist`

**现象**  
安装 ROM 时报 `Uncaught RuntimeError: null function`，调用栈指向 `wasm_init_with_rom → io_system::exist → hle::lib_manager::load`。

**根因**  
`wasm_init_with_rom` 没有调用 `install_rom`，直接跳进了需要完整系统初始化的代码路径。

**修复**  
- `main.cpp`：正确调用 `eka2l1::loader::install_rom` 完成 ROM 提取，再调用 `g_state.symsys->set_device(0)` 初始化系统。
- `shell.html`：上传目标路径改为 `/eka2l1/rom_upload.rom`。

---

### 错误 2：`Uncaught unwind` + SIS 安装后 App 列表为空

**现象**  
点击 Run 报 `Uncaught unwind`；安装 SIS 后 App 列表无内容。

**修复**  
- `main.cpp`：移除多余的 `wasm_main_loop()`；实现 `wasm_install_package`、`wasm_get_app_list`、`wasm_launch_app`。
- `shell.html`：`startEmulation/pauseEmulation` 改用 `ccall('wasm_set_paused', …)`；添加 App 列表 UI 与关闭按钮。

---

### 错误 3：ROM/SIS 数据不持久化

**现象**  
刷新后 ROM 和 SIS 数据全部丢失。

**修复**  
- `shell.html`：接入 IndexedDB（IDBFS）持久化；设置 `moduleConfig.noInitialRun: true`；在 `createEKA2L1Module().then()` 里挂载 IDBFS 并 `FS.syncfs(true)` 加载数据；安装完成后调 `syncToIDB()` 保存。

---

### 错误 4：`TypeError: Cannot read properties of undefined (reading 'FS')`

**根因**  
IDBFS 挂载代码放在 `preRun` 里，此时 `mod.FS` 还未就绪。

**修复**  
将 IDBFS 挂载移至 `createEKA2L1Module().then()` 回调中执行。

---

### 错误 5：`rom init failed code -3` + 控制台无输出

**现象**  
ROM 安装返回 `-3`，但浏览器控制台没有任何 C++ 日志。

**修复**  
- `main.cpp`：在 `main()` 中调用 `eka2l1::log::toggle_console()` 把 spdlog 输出接入浏览器 console。
- 识别到 EKA2（S60 3rd gen+）设备需要同时提供 `.rom` + `.rpkg` 两个文件；重构 `wasm_init_with_rom(rom_path, rpkg_path)`，按文件是否存在分支调用 `install_rpkg` 或 `install_rom`。
- `main.cpp`：强制 `g_state.conf.storage = "/eka2l1"`（IDBFS 挂载点），避免路径不一致。
- `shell.html`：添加 RPKG 第二文件选区，双文件齐备后自动触发安装。

---

### 错误 6：`Aborted()` / `std::bad_cast` in `dynamic_ifile`

**现象**  
读取 `product.txt`、`sw.txt` 时崩溃，调用栈显示 `std::basic_filebuf::underflow()` 触发 `__throw_bad_cast`。

**根因**  
`common::dynamic_ifile` 使用 `std::ifstream`，其底层 `std::basic_filebuf` 在 Emscripten libc++ 中依赖 locale facets，导致 `bad_cast`。

**修复**（`dynamicfile.h` / `dynamicfile.cpp`）  
将 `std::ifstream stream_` 替换为 `FILE *fp_`，用 `fopen/fread/fseek/fgetc/feof` 重写所有 I/O 操作，彻底绕开 `std::basic_filebuf`。

---

### 错误 7：`Uncaught $std::__2::locale::locale()` in `kernel_system::reset()`

**现象**  
RPKG 安装成功后，`set_device(0)` → `kernel_system::reset()` 第 186 行构造 `std::locale()` 崩溃。

**根因**  
Emscripten WASM 中 `std::locale` 的**默认构造函数**试图加载系统 locale 数据，而 WASM 环境不存在 locale 数据库。

**修复**（`kernel/src/kernel.cpp`、`utils/src/uchar.cpp`、`utils/include/utils/uchar.h`、`kernel/src/svc.cpp`）

1. `kernel.cpp`：`#ifndef __EMSCRIPTEN__` 跳过 `locale_` 构造，WASM 下保持 `nullptr`。
2. `uchar.h`：函数参数由 `std::locale &ln` → `std::locale *ln`（指针可为空）。
3. `uchar.cpp`：`ln == nullptr` 时用 `std::iswcntrl / std::towupper / std::towlower` 等 C locale 函数替代。
4. `svc.cpp`：6 处调用由 `*kern->get_current_locale()` 改为 `kern->get_current_locale()`（传指针，不解引用）。

---

### 错误 8：`locale(const &)` 拷贝构造也崩溃

**现象**  
改用 `std::locale::classic()` 后，拷贝构造函数 `locale(const locale&)` 同样崩溃。

**结论**  
Emscripten 中 `std::locale` 的任何构造路径均不可用。最终方案：WASM 下 `locale_` 保持 `nullptr`，`uchar.cpp` null 检查走 C locale 路径（见上）。

---

### 错误 9：`operation does not support unaligned accesses` in yaml-cpp

**现象**  
`set_device` → `config->serialize()` → `YAML::Emitter::WriteIntegralType<int>` → `std::stringstream` 构造 → `std::ios_base::init()` → `std::locale::locale()` 崩溃。

**根因**  
**任何 `std::stringstream`/`std::ostringstream` 的构造**都会调用 `std::ios_base::init()` → `std::locale::locale()`，这是 Emscripten libc++ 的系统性限制。

**修复**（`external/yaml-cpp/include/yaml-cpp/emitter.h`、`traits.h`）

1. `emitter.h`：`WriteIntegralType<T>` 在 `__EMSCRIPTEN__` 下用 `snprintf("%lld")` 替代 `std::stringstream`。
2. `emitter.h`：`WriteStreamable<T>` 在 `__EMSCRIPTEN__` 下用 `snprintf("%g")` + 特殊值字符串（`.nan`/`.inf`）替代。
3. `traits.h`：`streamable_to_string<Key,true>` 在 `__EMSCRIPTEN__` 下返回空字符串（仅用于 key-not-found 错误信息，不影响功能）。

---

## 涉及文件汇总

| 文件 | 改动内容 |
|---|---|
| `src/emu/web/src/main.cpp` | 日志接入 console；双文件安装逻辑；storage 路径固定 `/eka2l1` |
| `src/emu/web/shell.html` | RPKG UI；IDBFS 持久化；App 列表；错误码解码 |
| `src/emu/common/include/common/dynamicfile.h` | `std::ifstream` → `FILE *` |
| `src/emu/common/src/dynamicfile.cpp` | 用 C fopen/fread 重写 |
| `src/emu/kernel/src/kernel.cpp` | WASM 下跳过 `std::locale` 构造 |
| `src/emu/kernel/src/svc.cpp` | locale 参数改为指针传递（6 处） |
| `src/emu/utils/include/utils/uchar.h` | `std::locale &` → `std::locale *` |
| `src/emu/utils/src/uchar.cpp` | null locale 时用 `<cwctype>` C 函数 |
| `src/external/yaml-cpp/include/yaml-cpp/emitter.h` | WASM 下 WriteIntegralType/WriteStreamable 改用 snprintf |
| `src/external/yaml-cpp/include/yaml-cpp/traits.h` | WASM 下 streamable_to_string 返回空字符串 |
| `CMakeLists.txt`（根） | EMSCRIPTEN 下加 `FMT_USE_CONSTEVAL=0` |

---

## 构建命令

```bash
# 配置（在 build_wasm_test 目录）
emcmake cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEKA2L1_BUILD_TOOLS=OFF \
  -DEKA2L1_BUILD_TESTS=OFF \
  -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# 编译
cmake --build . --target eka2l1_wasm -j4

# 测试服务
cd build_wasm_test/bin
python3 ../../src/emu/web/serve.py 8080 .
```
