# 2026-09 yeatse → Web 同步

来源：本机 `EKA2L1-yeatse` 的 `ios-next`，截至 `7d3166551`（2026-09-07）。
增量基线为上次 Web 同步使用的 `ae75947b8`，并补齐回归测试发现的基线依赖遗漏。
这是面向 Web 的代码移植，不是完整合并 iOS 分支。

## 同步内容

- 内核相对定时器 tick 队列、线程/内存/调度修复，S60v3 MR 支持及 ROM/E32Image 校验。
- 窗口 surface 生命周期、EGL/视频发布与合成分离，GDI 数据对齐、OpenVG 路径和 GLES2 属性/着色器修复。
- 字体内容识别、ROM linked fonts、字体匹配和图集打包；SVG 内嵌图片及 switch 元素支持。
- 包安装/升级/卸载、统一归档读取、RPKG 安装优化、大小写保留的文件路径解析。
- 音频流进度与完成时序、相机公共像素处理、系统服务和 IPC 修复。
- 同步相关测试与 Symbian 预编译补丁，新增 qjpeg 补丁并打包到 Web 资源。

## Web 适配

- 保留现有浏览器 WebAssembly JIT 和 CPU 时间片策略；未移入上游整套 dyncom 性能重写，只移入兼容的异常指令/Thumb 断点修复。
- 保留 MEMFS/IDBFS、浏览器音频/相机、WebGL2、WASM 内存分配和现有 J9 接口。
- Web 流式 RPKG 安装器也应用目录创建缓存。
- 新字体图集保留小字号黑白栅格化、灰度纹理和 CJK 占位字过滤；内嵌黑白字形按应用要求转换成 8 位覆盖率。
- 保留 miniz 给现有 J9 调用，同时引入 zlib、xz、libarchive；更新 miniBAE 和 lunasvg 子模块。
- 构建脚本优先使用完整 emsdk，避免 PATH 中的 Homebrew Emscripten 与已有 SDK 缓存混用。
- 不接入 iOS/Qt/Android 界面、AirPlay、手柄 UI 或 Bonjour 原生蓝牙发现；Web 继续使用原有网络后端。

## 构建与回归

Web Release：

```sh
bash buildscript/build_release.sh
```

产物位于 `build_wasm_release/bin/`。

本机共享核心测试使用 x86_64/Rosetta，以匹配仓库现有 FFmpeg 预编译库；关闭 Homebrew 仅提供 arm64 的可选 FreeType PNG/Brotli 依赖：

```sh
cmake -S . -B build/yeatse-tests-x64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DEKA2L1_BUILD_TESTS=ON -DEKA2L1_BUILD_FRONTEND=OFF \
  -DEKA2L1_BUILD_TOOLS=OFF -DEKA2L1_BUILD_PATCH=OFF \
  -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF -DEKA2L1_CPU_DYNCOM_ONLY=ON \
  -DFT_DISABLE_PNG=ON -DFT_DISABLE_BROTLI=ON
cmake --build build/yeatse-tests-x64 --target ekatests -j6
DYLD_FRAMEWORK_PATH="$PWD/src/external/sdl2/macos" \
  ctest --test-dir build/yeatse-tests-x64 --output-on-failure
```

验证结果：

- Emscripten 5.0.7 Release 编译、链接和资源打包成功。
- CTest 通过：226 个测试、5871 条断言，包括新增 CJK 占位字过滤和黑白字形转灰度回归。
- 浏览器实测 5320 ROM/RPKG：流式安装完成、设备初始化返回 0；计算器 JIT 模式输入 `12 + 3`，得到 `15`，选项菜单正常展开；解释器模式输入 `45` 后删除一位得到 `4`，未出现访问违规或应用退出。
- 回归页使用独立 MEMFS，重复刷新安装不会读取或覆盖正常启动页的设备存档。
- `git diff --check`、构建脚本语法检查、回归页 JavaScript 和测试服务器 Python 语法检查通过。

计算器修复：整数 Publish & Subscribe 属性的 `ndata` 未初始化，读取新定义的
Avkon QWERTY 模式属性（类别 `0x101F876E`、键 `4`）可能得到旧堆内容。
计算器因此误入 QWERTY 输入分支，而 5320 固件没有相应键盘映射，最终在
`ptiengine.dll+0x4E6A` 向空指针偏移 `0x4` 写入并退出（`KERN-EXEC 3`）。
现在新属性显式初始化为零，遵循数字键盘路径；保留全部新版媒体补丁。
新增测试使用填充 `0xA5` 的存储，并在写入非零值后原址重建属性，验证两次初始读取均为零。

本机这份 5320 固件的语言列表不含简体中文，选择中文会回退到固件默认语言；
中文字体转换与回退由专项测试覆盖，未将英文固件画面视为中文运行验证。

## 保留的回归页

源码：`src/emu/web/pages/smoke.html`，随 Web 构建复制到 `build_wasm_release/bin/smoke.html`。
支持 ROM/RPKG 文件选择、设备语言与 CPU 模式选择、应用启动、屏幕键盘和日志导出。
应用退出时会在状态栏提示，并记录错误上下文。

```sh
python3 buildscript/serve_regression.py
```

打开 <http://127.0.0.1:18091/smoke.html>。服务器仅监听本机；也可以指定其他端口和产物目录：

```sh
python3 buildscript/serve_regression.py 18093 --directory build_wasm_test/bin
```

不选择文件时，页面读取所服务目录中的 `rom/SYM.ROM`、`rom/SYM.RPKG`。
当前本机 Release 产物中已有指向 5320 固件的链接；固件文件与这些本地链接不提交到仓库。
