# J2ME MIDlet 启动链路（S60v3 / WASM）

本文档记录 EKA2L1 WASM 构建中，S60v3 ROM 下 J2ME MIDlet 的安装、注册、启动流程，
以及为解决「跨页面注册丢失」问题所做的全部改动。

---

## 一、背景：S60v3 Java 运行时架构

一个 Java-capable 的 S60v3 ROM 通常包含以下组件：

| 组件 | 路径 | 作用 |
|------|------|------|
| MIDP2 静默安装器 | `Z:\sys\bin\midp2silentmidletinstall.exe` | 从 JAD/JAR 把 MIDlet 注册进 AppArc。需以 `systemamscore.exe` 为 creator 调用（校验 SID 0x10203636） |
| AMS 核心 | `Z:\sys\bin\systemamscore.exe` | Java 生命周期/安全/注册表服务器。由 **`SystemAMS.EXE`** 以 **` -boot`** / **` -proxy`** 拉起；`-boot` 才会挂出 `!MIDP.SystemAMS.SystemAMS` |
| AMS 蹦床 | `Z:\sys\bin\SystemAMS.EXE` | 挂出 `!SystemAMSTrader.Public` / `.Private`，再 spawn 两份 SystemAMSCore |
| IBM J9 VM | `Z:\sys\bin\j9midps60.exe` | 5320 上真正执行字节码的 VM。参数：`-jad` `-jar` `-msid` `-msin` `-app <类名>`。**`-app` 必须带 MIDlet-1 第三段类名**（裸 `-app` 会 `usage()` → Exit(1)）。**不要** `-path`、**不要** `-event AMS.StartApp`（表空时走事件路径 → Exit(1)）、**不要** APA env slot 1 |
| MIDP2 运行时 | `Z:\sys\bin\midp2runtimev2.dll` | J9 / MIDP2 运行时 DLL |
| MIDP2 启动器 | `Z:\sys\bin\midp2midletlauncher.exe` | 独立启动器 exe（部分 ROM 有；**5320 没有**） |
| 文件识别器 | `Z:\sys\bin\StubMIDP2RecogExe.exe` | **不是 VM**。AMS 蹦床，连 `!MIDP.SystemAMS.MIDP2` 发 opcode 1 后立刻 `User::Exit(IPC)`。退出 ≠ 游戏退出，也不等于 J9 已起来 |

套件目录：`C:\private\102033E6\MIDlets\<uid>\`（JAR/JAD）；AMS 数据：`C:\system\data\midp2\systemams\`。

### 启动路径（5320 / IBM J9 优先）

1. **AMS + StubMIDP2**（首选）：`SystemAMSCore.exe` 命令行必须是 ROM TLitC **` -boot`**（前导空格），把套件拷到 `C:\private\102033E6\MIDlets\<uid>\`。
   蹦床 **先** `CreateSession("!SystemAMSTrader.Public")` 再连 `!MIDP.SystemAMS.MIDP2` 发 opcode 1。
   Trader 由 **`SystemAMS.EXE`**（SID `0x200159D7`）`StartServer` 挂出 Public/Private，再 `RProcess::Create` 两份 `SystemAMSCore.exe`（` -boot` / ` -proxy`）。只起 `SystemAMSCore` 时 Public/Private 都不存在，蹦床 `return -1`。
   Unicode 描述符必须用奇数 TCardinality + **每字符 1 字节**（ROM 的 `HBufC::NewL` 未压缩路径）。
   文本 `"jad" O "jad"` **不能**携带 OpaqueData，空 opaque 会 `ReadInt32L` → `User::Exit(-25)`，因此不再作为回退。
   不要从随机 guest 线程发宿主 opcode 1（会把 LLE AMS 弄挂）。
   蹦床把 launch IPC 发给 AMS，AMS 再 spawn `j9midps60.exe`。退出跟踪绑在 J9 上。
   5320 上 **不要**在 StubMIDP2 失败后直启 J9（`j9_23_midp2ams` export 2 会 `User::Exit(1)`）。
2. **J9 直启**（回退）：StubMIDP2 未能拉起 J9 时，直接 `j9midps60.exe -jad … -jar … -suitename … -mid … -path …`。
   `j9_23_midp2ams` export 2 在 `MIDP2RuntimeV2.dll` 工厂失败时会 `User::Exit(1)`。
3. **独立启动器**（其它 ROM）：`midp2midletlauncher.exe`。

---

## 二、安装流程（`j2me::install_for_midp2`）

```
install(jar_path)
  └─ install_for_midp2(sys, applist, path, entry, cb)
       ├─ get_app_entry()          // 解析 JAD/MF，提取 name/vendor/version/icon
       ├─ extract_icon_to_store()  // 提取图标到宿主存储
       ├─ 宿主侧注册               // applist->add_entry() —— 始终执行，即使 ROM 无运行时
       ├─ build_midp2_storage_dir  // E:\system\midp\<vendor>\<name>\<ver>\
       ├─ 拷贝 JAR/JAD 到 storage  // 持久化，跨重启可恢复
       ├─ prepare_midp2_environment()
       ├─ restage_to_preinstall()  // 拷到共享目录 E:\system\data\midp2\preinstall\m.{jar,jad}
       ├─ 修正 JAD 中 MIDlet-Jar-URL  // 换成本地 m.jar
       ├─ spawn systemamscore.exe (creator)
       └─ spawn midp2silentmidletinstall.exe (子进程，cmdline = JAD 路径)
            └─ installer->logon(cb) // 异步：安装器退出时触发 cb
```

### 关键设计

- **宿主侧注册先行**：MIDlet 总是立即加入宿主 j2me app list，即使 ROM 无运行时也能显示。
- **per-MIDlet storage**：JAR/JAD 永久存在 `E:\system\midp\<vendor>\<name>\<ver>\`，
  使 MIDlet 跨模拟器重启可恢复、可重注册。
- **creator SID 校验**：`midp2silentmidletinstall.exe` 校验 `User::CreatorSecureId()`，
  拒绝非 SystemAMSCore（SID 0x10203636）调用者，故必须以 AMS core 为父进程。

---

## 三、核心问题：跨页面注册丢失

### 现象

从库页面 (`index.html`) 跳到运行页面 (`run.html`) 时，整个 WASM 模块重新加载
→ 模拟系统重启 → **AppArc 的内存注册表被清空**。

静默安装器在上一会话中注册的 MIDlet 在新会话中丢失，导致
`Java apps in AppArc: 0`，启动返回 `-2`。

### 解决方案：启动时自动重新注册

当 `wasm_launch_midlet` 发现 MIDlet 不在 AppArc 中时：

1. 从持久化的 storage 目录重新拷贝 JAR/JAD 到 preinstall 目录
2. 修正 JAD 中的 `MIDlet-Jar-URL`
3. 重新 spawn `systemamscore.exe` + `midp2silentmidletinstall.exe`
4. 让安装器在**当前会话**中把 MIDlet 重新注册到 AppArc
5. 返回 `-100` 给 JS 层，JS 轮询 `midletInstallStatus()` 后重试启动

---

## 四、改动详情

### 4.1 `j2me::reregister_midlet()`（新增）

**文件**：`src/emu/j2me/src/interface.cpp`、`src/emu/j2me/include/j2me/interface.h`

重新运行 ROM 静默安装器，把已安装的 MIDlet 注册到当前会话的 AppArc。
复用 `restage_to_preinstall()` 把 JAR/JAD 重新暂存，再以 AMS core 为父进程
spawn 静默安装器。返回 true 时安装器已 spawn，`exit_cb` 异步触发。

### 4.2 `j2me::check_launch_capability()`（新增）

**文件**：同上

检查 ROM 是否包含运行 MIDlet 所需的 Java 运行时组件：
- 独立启动器 exe（`midp2midletlauncher.exe`/`midletlauncher.exe`）
- 或 AppArc 非原生路径（`systemamscore.exe` + `midp2runtimev2.dll`）

### 4.3 `wasm_launch_midlet`（重写）

**文件**：`src/emu/web/src/main.cpp`

双策略启动：

```
wasm_launch_midlet(uid)
  └─ Strategy 1: AppArc 注入（仅注册，不拿 StubMIDP2 当 VM）
       ├─ 扫描 / 注入非原生 Java 项
       ├─ 命中 → j2me::launch() → SystemAMSCore -boot + j9midps60.exe
       └─ 未命中 → reregister_midlet() → 返回 -100 (JS 重试)
  └─ Strategy 2: 再次 j2me::launch()（无 AppArc 时仍可直启 J9）
  └─ 全失败 → 返回 -2
```

返回 `-100` 时表示「重新注册已触发，请等待后重试」。

### 4.4 `run.js` 处理 -100

**文件**：`src/emu/web/pages/js/run.js`

检测到 `-100` 时：
- 显示「正在注册 MIDlet…」遮罩层（含秒数倒计时）
- 保持模拟器运行，让 guest 安装器推进
- 每 500ms 轮询 `midletInstallStatus()`
- 安装器成功退出（status=2）→ 重新 `launchMidlet()`
- 安装器失败（status=-4）→ 显示 `reregisterFailed` 错误
- 45s 超时 → 显示超时错误

### 4.5 安装器退出诊断

**文件**：`src/emu/web/src/main.cpp`

`wasm_install_midlet` 和 reregister 回调均记录安装器退出类型/原因/分类，
便于诊断 ROM 安装器为何 panic 或卡住。

### 4.6 图标提取修复

**文件**：`src/emu/j2me/src/common.cpp`

- 图标相对路径改用正斜杠 `j2me/<name>_<ver>_<icon>`，兼容 WASM/Unix
  （旧反斜杠形式在 WASM 上静默失败）
- 首次安装时 `j2me/` 子目录可能不存在，`fopen("wb")` 会失败，
  现先 `create_directories()` 创建父目录

### 4.7 `extract_midlet_icon_png()`（新增）

**文件**：`src/emu/j2me/src/interface.cpp`

当 install 时图标未写入宿主存储（WASM 首次安装目录缺失），
前端可实时从 per-MIDlet storage 中的 JAR 重新解析图标字节，无需重装。

### 4.8 前端诊断工具

**文件**：`src/emu/web/pages/js/boot.js`

新增浏览器控制台辅助函数：

- `EKA2L1.checkMidletLaunch()` — 检查启动器可用性，返回诊断字符串
- `EKA2L1.diagnoseJ2ME()` — 全面 J2ME 就绪诊断（运行时清单、关键二进制、AppArc 计数）
- `EKA2L1.probeJavaInventory()` — 列出 ROM 中 Java 相关二进制
- `EKA2L1.probeLs(path)` / `EKA2L1.probeCat(path)` — 目录列表/文件查看

### 4.9 `index.js` 安装/卸载改进

**文件**：`src/emu/web/pages/js/index.js`

- MIDlet 注册后立即 `refreshApps()`，即使 ROM 安装器后续卡住也能显示
- 安装器超时按非致命处理（MIDlet 已注册），显示 toast 而非阻塞对话框
- 卸载时区分 j2me/package，清除图标缓存
- 安装器等待超时从 180s 缩短到 60s

### 4.10 i18n 字符串

**文件**：`src/emu/web/pages/js/i18n/en-US.js`、`zh-CN.js`

新增：`registeringMidlet`、`registeringHint`、`registeringHintSecs`、
`reregisterFailed`、`noJavaLauncher`（含缺失 DLL 详情）、`midletNotRegistered` 等。

---

## 五、启动流程时序图

```
用户点击 MIDlet
     │
     ▼
run.js: launchMidlet(uid)  →  wasm_launch_midlet
     │
     ├─ AppArc 注入（会话内非原生项，不 spawn StubMIDP2）
     │
     ▼
j2me::launch()
     ├─ restage preinstall + C:\private\102033E6\MIDlets\<uid>\
     ├─ SystemAMSCore.exe -boot（及 AMSDbServer / javaregistry / JavaRedirServer）
      └─ opcode 1 skip Complete(0) 后宿主 `spawn_j9_with_ams()`
            └─ j9midps60.exe -jad C:/j.jad -jar C:/j.jar -msid … -msin 1 -app <类名>
                  （正斜杠；不要 `C:\j.jad` / `-path` / `-event` / APA。见 §十）
                  └─ 退出回调绑在 J9，不绑 StubMIDP2
```

---

## 六、调试方法

### 6.1 控制台诊断

启动设备后，在浏览器控制台运行：

```js
EKA2L1.diagnoseJ2ME()
```

输出包括：
- Java 运行时清单（`Z:\sys\bin\` 下的 Java 二进制）
- 关键 MIDP2 二进制存在性检查
- AppArc 中 Java 应用计数

### 6.2 关键日志

| 日志 | 含义 |
|------|------|
| `MIDP {} JAR staged at {} and the guest Java installer was started` | 安装器已 spawn |
| `Guest MIDP installer exited: type={}, reason={}, category={}` | 安装器退出信息 |
| `MIDlet '{}' not in AppArc; reregister triggered` | 触发了重注册 |
| `Reregister installer exited: type={} reason={} cat={}` | 重注册安装器退出 |
| `Reregister complete, retrying launch` | JS 侧重注册完成，重试启动 |

### 6.3 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| `midp2runtimev2.dll [MISS]` | ROM 无 Java VM | 换 Java-capable ROM |
| `Java apps in AppArc: 0` | 注册丢失（重启） | 触发重注册（-100 路径自动处理） |
| 安装器 `reason` 非 0 | 安装器 panic | 查 category，可能是 ROM 不兼容 |
| 进程异常退出：j9midps60 … **KERN-EXEC 3** | 未处理的 CPU 异常。常见原因：16KB 栈被 `j9_23_midp2ams.dll` 撑爆；或 AMS 的 `!MIDP.SystemAMS.SystemAMS` 还没起来 J9 就 Connect 了空 handle | 已：J9 栈改为 256KB；spawn 前泵主循环直到 AMS server 就绪 |
| 进程异常退出：j9midps60 … None **1** | `j9_23_midp2ams` export 2：`MIDP2RuntimeV2.dll`（及 V1 回退）Load/工厂失败。直启没有 AMS launch context | 已：StubMIDP2 + env slot 1 未压缩 `CApaCommandLine`（奇数 TCardinality + Latin-1 字节）+ opaque `(uid, 1)`；预加载 RuntimeV2/JTWI；失败再直启并带 `-mid` |
| StubMIDP2 `reason=-25`（KErrEof） | slot 1 用了压缩 Unicode，或 opaque 不足 8 字节导致 `ReadInt32L` EOF | 已：`to_guest_env_slot()` + 8 字节 opaque |
| SIS 图标全丢 / 卸载列表为空 | 见 §八 回归修复 | 重新构建 WASM；图标缓存会升到 v4 自动失效 |

---

## 八、回归修复（SIS 图标 / 卸载列表）

`150275b73` 引入 J2ME 卸载与图标路径时带了两处回归：

### 8.1 卸载列表为空（`wasm_get_packages`）

追加 MIDlet 时调用了 `json.pop_back()`，却**没有先**写入收尾的 `]`。
`pop_back()` 实际删掉了最后一个 SIS 包 JSON 的 `}`，整段变成非法 JSON，
前端 `JSON.parse` 失败后显示空列表。

修复：先 `json += "]"`，再按 `wasm_get_app_list` 同样方式 pop 并追加。

### 8.2 SIS 图标消失（`wasm_get_app_icon`）

用 `uid & 0x7E000000` 判断「是否为 J2ME 虚拟 UID」。该掩码会命中大量
第三方 SIS UID（常见 `0xAxxxxxxx` 也带 `0x20000000` 位），误走进 MIDlet
图标路径并返回 `null`。

修复：改为精确前缀判断 `(uid & 0xFF000000) == 0x7E000000`。
前端图标缓存升到 `eka2l1_icons_cache_v4`，清掉被误缓存为空的 SIS 图标。

---

## 九、启动失败：静默安装 UI 无人确认（UserCancel）

### 现象

MIDlet 能出现在程序库（宿主 j2me DB 已注册），但点击启动返回 `-2` / 重注册超时。
`EKA2L1.diagnoseJ2ME()` 常见 `Java apps in AppArc: 0`。

### 根因

5320 的 `midp2silentmidletinstall` 会拉起 SWInst + `ifeui` 确认未签名包。
库页面 / 无按键环境下确认对话框无人点，SWInst 返回 **`-30471 = KSWInstErrUserCancel`**，
AppArc 从不 `register_non_native_app`，启动路径（AppArc 非原生）无注册可用；
ROM 又没有独立的 `midp2midletlauncher.exe`，只能失败。

已有的 `midp2installerplugin.dll` bne→b 兼容补丁只能跳过其中一条 leave-info 路径，
挡不住 ifeui 交互取消。

### 修复

1. **`midp2installerplugin.dll` consent stub**：把未签名确认 helper 直接改成 `return 0`
   （不再调用 ifeui），避免头less 下 Leave / UserCancel。
2. **leave-info 补丁保留**：consent 返回 0 时跳过写入 `{category=902, code=6}`。
3. **`ifeui.dll` auto-accept**：其它安装路径仍会直接弹 ifeui 确认框；把对话框 Run
   入口 stub 成立即 `return 1`（当作软键确认），避免无人按键时的 `-30471` / Leave(-25)。
4. **AppArc 名称匹配**：同时匹配 `MIDlet-Name` 与 `MIDlet-1` 标题（忽略大小写）。
5. **卡住的安装状态**：若上一轮安装器挂起导致 `midlet_install_state` 一直为 1，启动时重置后再重注册。
6. 安装 / 重注册等待超时放宽到 90s。

> 曾尝试主循环自动注入 LSK/OK，但会与 ifeui 竞态触发 Leave(-25)，已撤回。
> 仅 NOP 掉 Run 调用点会让 Create 后的对话框状态不完整并挂起，已改为 stub Run 入口。

### 宿主侧 AppArc 注入（启动不再等 guest 安装器）

guest 静默安装即使补丁后仍可能 `-30471` 回滚。启动时改为：

1. 把 JAR/JAD restage 到 AMS 套件目录 `C:\private\102033E6\MIDlets\<uid>\`（**不再**放进 preinstall，以免 AMS `-boot` 自动安装）
2. 在 AppArc 内存表里注入非原生 Java 项（类型 `0x10210E26`，逻辑路径 = 套件 JAD）
3. `SystemAMS.EXE`（挂 Trader）拉起 `SystemAMSCore.exe` **` -boot`**（挂 MIDP2/SystemAMS），并拉起 `AMSDbServer` / `javaregistry` / `JavaRedirServer`
4. 等到 `!SystemAMSTrader.Public` **和** `!MIDP.SystemAMS.MIDP2` 都挂出后 spawn `StubMIDP2RecogExe.exe`，`CApaCommandLine`（`command_open`，未压缩 env slot 1，opaque=`uid,1`）
5. 蹦床把 launch IPC 发给 AMS 后会**立刻退出**（reason 0 = AMS 已接手）。宿主继续泵循环 / `-101` 等待 AMS 异步 spawn `j9midps60.exe`，**不要**把蹦床退出当成失败去直启 J9
6. 5320 上 StubMIDP2 失败一律 `-101` 重试，**不再**直启 `j9midps60.exe`（export 2 会 `User::Exit(1)`）
7. 注入失败才回退到静默安装器 `-100` 重试

`j9midps60` 的 CRT 用 `RProcess::CommandLine()` 拆 argc/argv。EKA2 HLE 曾把
UTF-16 命令行的 **字节数** 写进 TDes16 的 Length，ESTLIB 的 8-bit 拆分器在
第一个 UTF-16 NUL 处截断，export 2 随后空指针/坏路径 **KERN-EXEC 3**。
Length 必须按字符数设置。AMS 未挂出 `!MIDP.SystemAMS.SystemAMS` 时不得 spawn
J9，改为返回 `-101` 让 JS 泵主循环再试。

`TDayNameAbb` 会读 `KUidSystemCategory` / `KLocaleLanguageKey`（`0x101F75B6` /
`0x10208903`）里的星期缩写表。宿主若发布该 property 但表指针全是 0，euser 会跳过
ROM 内置表，然后 `ldr [0, day*4]` → **KERN-EXEC 3**（pc 在 `euser.dll+0xA734`）。
启动时改为填入 UTF-16 的星期/月份/日期后缀/am-pm/msg 表。

`j9_23_midp2ams.dll` 在 RAM `0x3FFF0028`（`.data+0x28`）上放了一个 `TDesC*`，
交给 `JvmNativePort.dll` 做 charset/名字 Compare。ROM 里这块 `.data` 全是 0，
而回调会在选项解析写下 HBufC 之前就被调用，于是 `TDesC::Compare` 对空指针
`ldr [r0]` → **KERN-EXEC 3**（`pc=euser.dll+0x5170`，`lr=j9_23_midp2ams.dll+0x4E15`）。
加载该 DLL 时把该槽位指到镜像字符串表里已有的 1 字符 `TLitC`（`L"\\"` / `L":"`）。
不要往入口附近那 12 字节 0 里种描述符：那是 ARM 静态析构表的字面量池，写进去会让
`DllMain(ProcessDetach)` 在 `j9_23_midp2ams.dll+0x4C` 对 `0x20` 做 `ldr` 再崩。

`StubMIDP2RecogExe` 空跑会立刻退出，前端会显示「进程异常退出」。5320 必须走 J9，不能把蹦床当成 VM。

consent stub 必须返回 **1**（软键确认）。返回 0 会被当成取消并写入 leave-info `{902, 6}`。
release 版 `midp2silentmidletinstall` 的 `iUntrusted = EPolicyNotAllowed` 也改为 Allowed。
缺失的 `KAknMemoryCardDialogUid` (0x101F467A) notifier 由宿主直接回答「介质就绪」。

---

## 七、构建验证

```bash
# WASM 构建
ninja -C build_wasm_release

# JS 语法检查
node -c src/emu/web/pages/js/run.js
node -c src/emu/web/pages/js/i18n/en-US.js
node -c src/emu/web/pages/js/i18n/zh-CN.js

# WASM 导出完整性（应 ≥ 50）
grep -o '_wasm_[a-zA-Z_]*' build_wasm_release/bin/eka2l1.js | sort -u | wc -l
```

### 本地调试服务

```bash
# 用带 COOP/COEP 头的本地服务器（不要用 8080，会和本机其它服务撞）
python3 src/emu/web/serve.py 18080 build_wasm_release/bin
# 然后访问 http://127.0.0.1:18080/ ，强刷后看 window.EKA2L1_BUILD_ID
```

---

## 十、5320 J9 真正出画：当前进度（未完成）

目标：S60v3 / Nokia 5320 ROM 上让 J2ME MIDlet **画出 LCDUI 画面**。
成功判据：`EKA2L1.redrawCount() > 0` 且帧来自 J9（`CreateSession: '!Windowserver' … from j9midps60`），不是 SWInst / 电话 UI。

测试套件：阿尔卑斯牧场物。host uid `0x20000004`，类名 `AlpsFarm`（JAD `MIDlet-1` 第三段）。
ROM：`5320 (S60v3)/SYM.ROM`，基址 `0x80000000`，`VA = 0x80000000 + file_offset`。

### 10.1 已钉死的架构

| 事实 | 说明 |
|------|------|
| 真 VM | `j9midps60.exe` UID3/SID `0x102033E6`。E32Main：`LoadLibrary("j9_23_midp2ams.dll")` 调 **export 2**，然后 `User::Exit(export2_return)` |
| Creator SID | 必须是 `0x10203636`（SystemAMSCore） |
| StubMIDP2 | **不是 VM**。SID `0x1020E724`，发 opcode 1 后立刻 Exit |
| 套件目录 | `C:\private\102033E6\MIDlets\<uid>\`（`m.jad`/`m.jar`） |
| Window server | 名是 **`!Windowserver`**。`!Java.Redir.Server` 只是 stdout/stderr |
| `[java-redir] opcode=-1` | JavaRedir **Connect**，不是 AMS opcode 1 |
| factory 失败码 | 已从 1 改成 **11**，避免和 Java `usage()` / startApp 的 Exit(1) 混淆 |
| J9 AllFiles | `apply_j9_allfiles_patch` 给 `j9midps60` AllFiles |

`j9.dll` XIP `0x818BBA10`（`j9main` `0x818BBB19`）。`j9vmall23.dll` header `0x818C1DC0`。
`j9_23_midp2ams` header `0x81A5B6D0`，code `0x81A5B748`，`.data` RAM `0x3FFF0000`（`+0x18` 文件对象，`+0x28` TDesC*）。

### 10.2 当前启动路径（宿主拉 J9）

真 AMS Find/Launch 会在 Find-on-miss cave 里 **FillZ → KERN-EXEC 3**，所以：

1. ROM 里 opcode 1 在 `0x81D20CE4` 改成直接 **Complete(0)**（跳过 Find/Launch）
2. Launch stub 在 `0x81D33860`（签名必须是 `70 b5`）
3. Stub Exit(0) `how=handoff` 后，宿主 **`spawn_j9_with_ams()`** 自己起 `j9midps60`

安装已经通：`irc=0, st=2`。

### 10.3 `Args.parse`（`0x81A81E30`）

`-app` / `-event` / `-jad` / `-jar` / `-msid` / `-msin` / `-path` / `-mid` / `-uid` **都吃下一个 token**。

| 命令行 | 结果 |
|--------|------|
| 裸 `-app` | `usage()` → `"Bad command line"` → Exit(1) |
| `-app -event AMS.StartApp` | `iAppClassName="-event"` |
| `-app AlpsFarm -event AMS.StartApp` | parse 过，走 AMS 事件路径（表空）→ Exit(1) |
| `-path C:\private\102033E6` | 拆出孤立 `-` → `"Unrecognized argument: -"` |
| `-jad C:\j.jad` | `C:` 后紧跟 `\j`，`-jad` 吃不到值 → usage() |
| `-app AlpsFarm`（无 `-event`） | parse 过，走本地 `startApplication` → `NativeFile.open` |

无 `-app` 时 J9 能活 5s+，会连 FileServer / Loader / FLogger / ecom / `!Java.Redir.Server`，**从不连 `!Windowserver`**。

### 10.4 JAR 打开链

```
AMS.run → initialize → initializeMIDletSuite → initializeUntrusted
  → MIDletSuite.loadUntrusted → loadManifest(String)
    → Jar.open → NativeFile.open → native _open
```

- `loadManifest` 只吃一条 JAR 路径
- Java 侧会 `replace('/', '\\')` 再交给 native
- JNI：`Java_com_symbian_j2me_midp_runtimeV2_NativeFile__1open` @ `0x81a5d493`
- `_open`：GetStringChars → `TPtrC16` → `*(0x3fff0018)` 的 vtable+0x20（mode=0）
- 返回 <0 → Java 抛**空消息** `java.io.IOException`

`NativeFile._open` 在 TPtrC 建完（`0x81a5d4ca`）和 Open 返回（`0x81a5d4e0`）打了一次性 BKPT，日志前缀 **`[j9-nf]`**（`Emulated.Stdout`，和 java-redir 同一通道）。

### 10.5 套件怎么暂存

`spawn_j9_with_ams()` 现在会：

1. 把 JAR 拷到 `C:\private\102033E6\j.jar`、`C:\j.jar`、`C:\m.jar`，以及类名 / 中文名别名
2. **不把中文 JAD 交给 J9**：重写成纯 ASCII（`MIDlet-Name: AlpsFarm`，`MIDlet-Jar-URL: j.jar`）
3. 命令行用 **`C:/j.jad` / `C:/j.jar`**（8 字符，正斜杠）
4. 额外把 JAR 字节放到截断残片 `C:\private\102` 和 `C:\private\102033E6\private\102`

中文「阿尔卑斯牧场物」只出现在 JAD 显示名和宿主 storage 目录（`make_safe_preinstall_name` 换成下划线）。**不是**当前打不开 JAR 的根因。

### 10.6 权威失败（BUILD `28a9fd8ec60e` / `7494d7a00a37`）

JAD 全路径能打开，JAR 被截断：

| FileServer 记录 | 含义 |
|-----------------|------|
| `Open raw='C:/private/102033E6/j.jad' exist=1` | `-jad` 完整，25 字符 |
| `miss raw='C:/private/102'` | `-jar` 同长度的 25 字符变成 **14 字符**，停在 `102033E6` 的 `E` |
| `no-dir raw='C:\Private\102033E6\/private/102'` | 同一残片再拼到 session 上 |
| `Open jxe=8194FF98 exist=1` | J9 自己的 cache，与套件无关 |

所以盘上的 `j.jar`（838890 字节）一直在，NativeFile 打开的不是那条。
`C:/j.jar` 是为躲开这条 14 字符截断；`C:\private\102` 是给仍走残片路径的回退。
**截至提交时仍未出画**，也还没稳定看到 `!Windowserver` from j9midps60。

### 10.7 不要再走的弯路

- 不要用「opcode 1 的 `-1` 改写成 `0`」当启动手段；当前是 ROM 里跳过 Find/Launch 直接 Complete(0)
- 不要重启 AMS 当主路径；不要 5320 冷直启 J9
- 不要给 CommandLine / ROM 注入裸 `-app`，或 `-app -event AMS.StartApp`
- 不要加 `-path`（孤立 `-`）；不要用 `C:\j.jad`（反斜杠）
- 不要把 TLitC* 直接传给 Append veneer
- 不要把 `!Java.Redir` 或 SWInst redraw 当成 J2ME 画面
- 不要在没看到 `[j9-nf] path=` / `fs Open` 之前再改命令行拼法碰运气
- Find-on-miss cave 的 FillZ 崩溃还没根治；重新打开真 Launch 前必须先解决它
- cave 不要按 32 字节找；不要用 32-bit B.W 跳过 load_all
- 不要 `bl super_ctor 0x81D2E05A` / `bl full ctor 0x81D2E258`

### 10.8 调试过滤

强刷确认 `EKA2L1_BUILD_ID`。控制台请贴：

```
[j9-nf]   wrote ASCII JAD / args= / path= / fs Open / fs miss / Open result
[java-redir]
CreateSession: '!Windowserver'
reason=
```

不要只贴 `java-redir`。等 20–30s 后还可 `EKA2L1.redrawCount()`、`probeJavaRuntime()`、`lastAppExit()`。

### 10.9 截断路径的就地重写（BUILD `74520ac70404` 起）

针对 §10.6 的权威失败（JAR 路径 25 字符被截成 14 字符 `C:/private/102`），
不再只靠盘上别名文件兜底，而是**在两处把路径本身修正**：

1. **`NativeFile._open` BKPT 钩子改为常驻**（`libmanager.cpp`）。
   旧实现命中一次后把原指令写回（一次性），J9 后续的 `_open`（JAD、JAR、jxe cache…）
   全部不可见。现在保留 BKPT，直接模拟被替换的 16 位指令
   （`movs r5,#0` / `movs r5,r0`）并 `pc += 2`。
2. **TPtrC16 就地重写**：命中 path 断点后，若路径匹配以下「截断套件路径」特征之一
   - `c:\private\102033e6` 的真前缀（长度 ≥ 14，如 `C:/private/102`）
   - `private\102033e6` 的真前缀（长度 ≥ 11，会话相对形态）
   - 含 `102033e6` 且以 `.ja` / `.j` 结尾（TBuf<40> 截断，如 `...m.ja`）

   则把 guest 缓冲区内容改写为 **`C:\j.jar`**（8 字符，任何截断缓冲都放得下），
   并把描述符 iTypeLength 的长度位改为 8（保留高 4 位类型）。
   之后的 size/seek 读到的都是真 JAR，而不是碰巧同名的别名。
   日志：`[j9-nf] rewrote truncated suite path '...' -> 'C:\j.jar'`。
3. **FileServer 层兜底**（`files.cpp`）：J9/SystemAMS 打开者打开不存在的文件、
   且名字匹配同一组截断特征（套件文件真前缀，或含 `102033e6` 且末段是无点纯数字）
   时，重定向到 `C:\j.jar` → `C:\private\102033E6\m.jar` → `C:\m.jar`（第一个存在者）。
   日志：`[j9-nf] fs redirect '...' -> '...' (truncated suite probe)`。

命令行拼法**没有改动**（仍是 `-jad C:/j.jad -jar C:/j.jar -msid … -msin 1 -app <类名>`），
`spawn_j9_with_ams()` 的盘上别名（`C:\private\102` 等）保留作第三层防线。

调试过滤在 §10.8 基础上新增两行：

```
[j9-nf] rewrote truncated suite path / fs redirect
```

### 10.10 退出行自带死因（BUILD `62625bc05e79` 起）

控制台经常被截断，`Leave code=` 行经常丢失。现在 `j9midps60` 的退出行
（`Java install process exit: … reason=1 …`）会直接附加 VM 临终上下文：

- `[last Leave: code=… pc=… lr=… seq=…]` —— svc.cpp `leave_start` 里记录的
  J9 进程内**最后一次** Symbian Leave（seq 是累计次数，可判断是否一路 Leave 到死）
- `[last NativeFile open: path='…' result=…]` —— `[j9-nf]` 钩子看到的最后一次
  `NativeFile._open` 路径与返回值（result=0 即打开成功）
- `[NativeFile hook never hit: …]` —— 钩子一次都没触发：要么 `j9_23_midp2ams`
  的 BKPT 没装上，要么 J9 在 `loadManifest` 之前就死了（Args.parse / usage 路径）

判读：
- `last Leave` 存在 + `NativeFile open result=0` → JAR 已打开，死在后续（AMS/
  安全/窗口服务器）；拿 Leave pc 对照 `j9vmall23.dll+offset` 反汇编。
- 无 `last Leave` + hook never hit → 命令行解析层就退了，回头查 `args=` 行。

### 10.11 BUILD `36c84203ffb4`：退出行升级 + ROM 地址解析结论

BUILD `62625bc05e79` 实测：`reason=1`，`[last Leave: code=-1 pc=0x8019E2FC … seq=2]`，
`[NativeFile hook never hit]`。把 pc/lr 对到本地 ROM（`/Users/zixing/Downloads/5320 (S60v3)/SYM.ROM`）：

- ROM header：base `0x80000000`，**pageable_start=0x67D000**（bytepair 逐页压缩），
  所以 `VA-0x80000000=文件偏移` 只对 **unpaged 段**成立；j9 系 DLL 全在 pageable 段，
  **直接从文件反汇编 j9 代码是无效的**。
- `0x8019E2FC` / `0x801AD55F` 都落在 **EUser.dll**（unpaged）。ARM 反汇编显示
  `0x8019E2F8: svc #0xde; 0x8019E2FC: bx lr` —— SVC 0xDE 就是 `leave_start`
  （EKA2L1 `svc.cpp` 的 `BRIDGE_REGISTER(0xDE, leave_start)`）。**Leave pc 只是泛型
  User::Leave 桩，不含死因**；真正的调用者在 pageable 段无法静态反汇编。
- 结论：需要 **guest 运行时数据**（哪个文件 KErrNotFound），静态分析到头了。

退出行因此再次升级：

- `[leaves total=N last: <code> @ <模块+偏移>] ring=[…]`（最近 4 次 Leave，
  pc 用 codeseg 表解析成 `模块+off`）
- NativeFile 钩子状态三态：`installed but never hit`（J9 死在 loadManifest 前）/
  `NOT installed`（j9_23_midp2ams 签名搜索失败）/ `open path=… result=…`
- **`[fs misses total=N ring=['路径'(err); …]]`** —— J9/AMS 打开者最近 4 次
  RFs 打开失败的路径与错误码（`files.cpp` 环形记录）。**这是下一次日志里
  定位 KErrNotFound 根因的关键字段**。

ROM 目录/反汇编脚本：`/tmp/rommap.py`（遍历 ROM 目录树，可按 VA 找模块；
unpaged 段可用 capstone 反汇编，pageable 段需先按 rom_page_idx 表 bytepair 解压）。

### 10.12 BUILD `36c84203ffb4` 实测：根因 = `midp2_trp.xml` 路径错位（已修）

退出行给出的关键数据：

```
[fs misses total=1 last='z:\Private\101F9F6C\security\midp2_trp.xml' err=-25]
```

排查（本地 `SYM.RPKG` 解析脚本 `/tmp/rpkglist.py`）：

- ROM 目录树里**没有**任何 101F9F6C / trp / private xml。
- RPKG（真机 Z: dump）里有 **`Z:\private\10203636\security\midp2_trp.xml`（4509 字节）**，
  内容是厂商证书信任根策略（Nokia Content Signing CA …）。
- 引用者：**`MIDP2SystemAMSv1_5.dll`**（UID3 = `0x101F9F6C`）内嵌该文件名的
  UTF-16 串（+0x2DB8C）——它按 `Z:\Private\<自己的UID>\security\midp2_trp.xml`
  构造路径，但文件实际发在 SystemAMS v2 的 `10203636` 目录。v1.5 探测
  KErrPathNotFound → Leave(-12/-1) → J9 Exit(1) 黑屏。
- 附带发现：j9_23_bluetooth_obex.dll / MIDP2BluetoothPushService.dll 也内嵌该 UID
  （作为 server SID，与本案无关）。

**修复**（`files.cpp`，BUILD `b27c7114f512`）：J9/AMS 打开者打开 `z:\private\101f9f6c\*`
不存在时，重定向到 `z:\private\10203636\*`（前缀长度 20，注意别写成 21）。
日志：`[j9-nf] fs redirect '…' -> '…' (MIDP2 v1.5 private dir -> v2)`。
泛化：v1.5 组件在该目录下找的**所有**文件都会落到 v2 副本。

⚠️ 第一版（`6cb1c361b9f5`）把重定向放在了 `is_it_avail` 计算之后，但
`z:\Private\101F9F6C\security\` 整个目录不存在，请求在更早的「目录存在性检查」
（no-dir 分支）就以 KErrPathNotFound(-25) 提前 return 了，重定向永远走不到
（症状：退出行仍报同一 miss err=-25）。第二版把重定向挪到 `file_dir` 计算**之前**。

同时补了 `apply_j9_nativefile_open_hook` 的失败日志：签名命中但 0x38/0x4E 偏移处
指令不符时打印实际字节（上一版静默失败，退出行只能显示 NOT installed）。

**前提**：必须安装 RPKG（ROM-only 时 Z: 没有该 XML，重定向后仍 404）。

### 10.14 BUILD `50634dfcdc5a` 实测：bt 指向 CenRep（已加日志待验证）

bt 数据：`bt=[euser+0x1272F; jclcldc11_23.dll+0xE99D; ...]`。离线分析
（`/tmp/rpkgdis.py`，RPKG DLL 是 **ROM image 格式**：0x78 字节 TRomImageHeader 后跟代码，
bt 偏移 +0x78 = 文件偏移；import thunk 为 ARM `ldr pc,[pc,#-4]; .word 绝对ROM地址`）：

- `euser+0x126F6` = `User::Leave` 内部；`euser+0x12722` = **`User::LeaveIfError`**
  （`subs r4,r0; bge skip; Leave(r4)`）。
- `jclcldc+0xE998` 处 `blx` thunk → 目标 **0x80561F51 = centralrepository.dll+0x79**
  （ROM VA 表映射），即 JCL CLDC 启动时调用 CenRep 客户端 API，返回 -12/-1 后
  `LeaveIfError` → J9 Exit(1)。
- EKA2L1 的 CenRep 从 `Z:\private\10202be9\<UID>.txt|.cre` 加载
  （`centralrepo.cpp: load_repo_adv`）；RPKG 里有 **285 个 repo ini**——文件在，
  但 J9 要的 UID 未必在列，或路径/mount 有问题。
- cenrep init 失败原有 `Repository not found with UID 0x...` 日志但是 TRACE 级，
  web 控制台看不到。

BUILD `3e3306a4ba50`：

- `centralrepo.cpp` init 失败日志 TRACE→**WARN**（web 控制台可见），成功路径加 INFO。
- 预期下一次运行能看到 `Repository not found with UID 0xXXXXXX`，据此合成缺失 repo
  或修加载路径。

注意：ROM 里 cenrep.dll 段是 pageable 压缩的，无法离线反汇编确认 +0x79 具体是哪个
导出（NewL vs GetInt），靠运行时日志定位。

### 10.15 离线钉死：缺失 CenRep `0x10274B74`（JCL epoch）

不必再等运行时 UID 日志。从 RPKG 抽出 `jclcldc11_23.dll`（ROM image，header `0x78`）：

| 项 | 值 |
|----|----|
| 唯一 `CRepository::NewL` | `jclcldc+0xE998` → import `0x1361C` → `centralrepository.dll+0x79` |
| 唯一 `Get` | `jclcldc+0xE9CE` → `centralrepository.dll+0x109`，`aKey=0`，`TDes16` |
| UID 字面量 | `ldr r0,[pc,#0xD8]` @ `0xE994` → `0x81982B4C` → **`0x10274B74`** |
| 源文件 | 邻接 ASCII `"epoch.jcl.cpp:175"` |
| RPKG | **289 个** `Z:\private\10202BE9\*.txt/.cre` 里**没有** `10274b74`；该 UID 只出现在 `jclcldc11_23.dll` |

真机上这块多半是 Java 首次启动写到 C: 的 factory persist，RPKG/ROM 不带。`CRepository::NewL` 对缺失 repo `LeaveIfError(-1)` → J9 `Exit(1)` 黑屏。同函数里 Get 失败只是 `return 0`（JNI 走默认时区），**不会** Leave。

**修复**（`centralrepo.cpp` / `repo.cpp`）：

1. 仓库缺失且（UID=`0x10274B74` **或** 调用者是 j9/java/midp/systemams）时，合成内存 stub，不再 `KErrNotFound`。
2. `0x10274B74` 预置 key `0` = UTF-16 `"GMT"`（合法 Java `TimeZone` id）。
3. init / Get miss 日志带进程名：`Repository not found with UID 0x… from …`、`CenRep Get miss repo=… key=… from …`。
4. 原生进程要的其它缺失 repo 仍返回 not found，避免把可选探测变成“仓库存在”。

调试时请确认：

```
CenRep: Repository not found with UID 0x10274B74 from j9midps60…; synthesizing JCL epoch stub
```

之后不应再因这一枪 `Leave(-1)`。若仍 `reason=1`，看新的 `Get miss` / `fs misses` / `last Leave`。

### 10.16 BUILD `2810178cad7a` 实测：`RDir::Open` Leave(-12) + 空 `java.properties` 挡路

退出行：

```
reason=1 [leaves total=1 last: -12 @ euser.dll+0x3494 lr=euser.dll+0x126F7]
bt=[euser.dll+0x1272F; efsrv.dll+0xDD9; efsrv.dll+0xE1F; j9vmall23.dll+0x436C5]
[NativeFile hook NOT installed]
```

离线：

- `j9vmall+0x436C0` `blx` → `efsrv+0xDE6` = **`RDir::Open` / `RFs::GetDir`**（opcode `0x31` = `fs_msg_dir_open`）。
- `efsrv+0xDD4` 对 IPC 结果 `LeaveIfError`；缺目录时 J9 直接死，JNI 包装自己并不 TRAP。
- NativeFile 钩子没装上是因为死在 `j9vmall` 里，`j9_23_midp2ams` 还没加载——不是签名搜失败。
- 退出行没有 `fs misses`：旧 `open_dir` 失败回 `-25` 且不记 ring。

更狠的是先前的 IVE overlay：

- RPKG **有** `Z:\resource\ive\bin\java.properties`（35220 字节，bootstrap 路径）。
- 旧逻辑把整个 `Z:\resource\ive\bin\*` 映射到 C:，而 `prepare_midp2` 在 C: 上建了**空** `java.properties`。
- J9 读到空配置 → `java.home` / bootstrap lib 路径是空的 → `GetDir` 打到不存在的目录 → Leave(-12)。

**修复**：

1. overlay 只改写 `java_*` 语言变体，**不再**挡 `java.properties`。
2. `prepare_midp2` 用 ROM 里那份覆盖空的 C: stub；只给 `java_en*.properties` 留空文件。
3. `open_dir` 对 J9：打 `[j9-nf] fs DirOpen`、记 fs-miss ring、缺目录则在 C: 建好再重试；失败改回 `-12`（对齐真机）。
4. FileServer `Entry` miss 也进 ring。

调试请看：

```
Provisioned java.properties from ROM
[j9-nf] fs DirOpen
```

### 10.17 BUILD `c4bbbc3b8f76` 实测：`Entry C:\Private\102033E6\0` ×5

DirOpen 已通（`jclCldc11\ext`、`nokiaextcldc`）。J9 仍 `reason=1`，无 Leave，NativeFile 钩子仍未装（死在 midp2ams `_open` 之前）。

```
[fs misses total=5 last='C:\Private\102033E6\0' err=-12]
```

这是 **`RFs::Entry`**（不是 Open）。`findSuiteHome` 去探 `C:\Private\102033E6\<msid>`。

命令行仍是 `-msid 0x20000004`。`TLex::Val` 按十进制读，遇到 `x` 停住 → **suite id = 0** → 连探 5 次 `...\0` → Java 侧 `Exit(1)`。

**修复**：

1. `-msid` 改成十进制（`536870916`）。
2. 在 `C:\private\102033E6\{0, <dec>, <hex>, <HEX8>}\` 各放一份 `j.jar`/`m.jar`/`j.jad`/`m.jad`。
3. `Entry` 对「`102033e6` + 无点纯数字」缺项时建目录再查；日志改到 `[j9-nf] fs Entry`。

### 10.18 BUILD `c9c2eb869d8f` 实测：在套件目录里找 `Main.class`

`-msid` 已是十进制，但 findSuiteHome 仍用 `0`（无 AMS 注册时的默认槽）。真正的死因是 49 次：

```
no-dir ...\0\com\symbian\j2me\midp\runtimeV2\Main.class      err=-25
no-dir ...\0\com\symbian\j2me\framework\Framework.class
no-dir ...\0\com\symbian\j2me\midp\runtimeV2\Args.class
no-dir ...\0\java\lang\RuntimeException.class
```

这些类在 `0_j9_23_midp2ams_jxe.odc` 里，类型 **JXESL**，容器是 `j9_23_midp2ams.dll`（含 `java/lang`、`runtimeV2`、`com/ibm/oti/nokiaextcldc`）。

上一轮 DirOpen 给不存在的 `Z:\resource\ive\lib\jclCldc11\nokiaextcldc\` **造了空目录并返回成功**。J9 把空目录当成 exploded package，**不再加载 DLL 里的 JXE**，然后在套件 classpath 上找 `.class`，`KErrPathNotFound (-25)` 又中止回退。最后连 `RuntimeException` 都加载不了，`Exit(1)`。

**修复**：

1. DirOpen **不再**给 `resource\ive\lib` 造假目录；只给 `\private\` 建目录。
2. 启动时删掉已存在的空 `C:\resource\ive\lib\jclCldc11\nokiaextcldc\`。
3. J9 打开缺父目录的文件时回 `-12`（KErrPathNotFound）。注意 Symbian：`KErrNotFound=-1`，`KErrPathNotFound=-12`，`KErrEof=-25`。

### 10.19 BUILD `d979757d2c16` 实测：JAR 已开，缺 `-jcl`，GetDir Leave(-1)

```
Open/Entry C:\Private\102033E6\\j.jar exist=1
no-dir ...\0\java\lang\ClassNotFoundException.class -> notfound
… Main/Framework/Args/RuntimeException.class
Leave -1  bt=LeaveIfError; efsrv GetDir; j9vmall+0x436C5
```

JAR 路径通了。`Main`/`java.lang.*` 在 **`jclcldc11_23.dll` + `j9_23_midp2ams.dll` 的嵌入式 JXE**（`META-INF/JXE.MF`，`J9GetJXE`），不是松散 `.class`。

j9.dll 认 `-jcl:cldc11` / `-Xjcl:`，对应 `jclcldc11_23.dll`。宿主命令行没带 `-jcl`，JCL 不加载，classpath 找不到 `Main`，再 `GetDir nokiaextcldc`（ROM 无此目录）`LeaveIfError(-1)`。

上一轮「不要造 nokiaextcldc」让 GetDir 直接 Leave；造空目录能躲过 Leave，但没有 `-jcl` 仍然没有 `java.lang`。

**修复**：命令行加上 **`-jcl:cldc11`**；DirOpen 对缺失目录仍给空 listing（避免 GetDir Leave）。

### 10.20 BUILD `f591cca23b98` 实测：`-jcl` 进了 midp2ams Args，JCL DLL 名也不对

`-jcl:cldc11` 出现在 cmd 里，但 J9 仍在 `...\0\` 找 `Main.class`，无 Leave。

两件事：

1. **midp2ams Args.parse 不认识 `-jcl`**（选项表只有 `-jad/-jar/-msid/-app/…`）。整段被当成 Unrecognized → Java `usage()` → Exit(1)，而且 `-jcl` 到不了 j9.dll。
2. j9vmall 默认 JCL 库名是硬编码的 **`jclcdc11_23`**（`-Xjcl:` 旁边的字面量）。5320 ROM 文件是 **`jclcldc11_23.dll`**（多一个 `l`）。LoadLibrary 找不到，`java.lang.*` 的 JXESL 从未加载。

jcl 的 JXE.MF 里是 `containsJCLs1` + `vmOption-jcl:cldc11`；midp2ams 的 JXE 是 `containsJCLs0`，依赖这份 JCL。

**修复**：从命令行拿掉 `-jcl`；`Loader::LoadLibrary` 把 `jclcdc*` 映射到 `jclcldc*`。

### 10.21 别名可能没走到 Loader

`7c8c39706322` 仍在 `...\0\` 找 `Main.class`，日志里没有 `LoadLibrary aliased`。J9 还可能用 **RFs 打开** `jclcdc11_23.dll`（`iveLoadJxeFromFile`），或 session 相对路径 `C:\Private\102033E6\jclcdc11_23.dll`。

**再补**：`lib_manager::load` 入口改名；FileOpen/Entry 重定向到 `Z:\sys\bin\jclcldc11_23.dll`；spawn 时拷到 `C:\sys\bin\` 和 J9 private 目录。日志走 `[j9-nf] LoadLibrary` / `codeseg load` / `fs redirect`。

### 10.22 BUILD `404faf613eb8` 实测：JXE 从未注册，仍在套件目录找 `.class`

命令行已是十进制 `-msid`，无 `-jcl`，但 49 次 miss 仍是：

```
C:\Private\102033E6\0\com\symbian\j2me\midp\runtimeV2\Main.class
C:\Private\102033E6\0\com\symbian\j2me\framework\Framework.class
C:\Private\102033E6\0\com\symbian\j2me\midp\runtimeV2\Args.class
C:\Private\102033E6\0\java\lang\RuntimeException.class
```

这些类在 DLL 嵌入式 JXE（ZIP：`rom.classes` + `META-INF/JXE.MF`，magic `J99J`）里，不是松散 `.class`。

`j9vmall` dynload.c 认三种 odc `type=`：`JAR` / `JXE` / `JXESL`。ROM 的 `0_j9_23_midp2ams_jxe.odc` 是 **JXESL**，靠 `LoadLibrary` + **`J9GetJXE`**。EPOC 的 `RLibrary::Lookup` 只有序数；midp2ams 的 export 1 不是那个无参 getter，命名查找失败，JXE 不注册。JCL 同样没挂上，所以连 `java.lang.RuntimeException` 都走文件系统。

另一条坑：dynload 扫 **`%c:\resource\ive\lib\jclCldc11\ext\*.odc`**（`%c` = 系统盘 C:）。DirOpen 给缺失的 C: `ive\lib` 造空目录后，GetDir 是空列表，ROM 里的 odc 全被挡住。

midp2ams 内部还会把 `L"-jcl:cldc11:nokiaextcldc"` 传给 j9.dll。`-jcl:<config>[:options]` 的 `:nokiaextcldc` 会让 J9 去 `jclCldc11\nokiaextcldc\` 找 exploded 包。

**修复**：

1. spawn 时把 `Z:\resource\ive\lib\jclCldc11\ext\` 拷到 C:；从 `j9_23_midp2ams.dll` / `jclcldc11_23.dll` 抽出嵌入 ZIP，写成 `midp2ams.jxe` / `jcl.jxe`。
2. 覆盖 C: 上的 midp2ams odc 为 **`type=JXE` `name=midp2ams.jxe`**，并加一份 JCL 的 `0_jcl_jxe.odc`。dynload 走 `iveLoadJxeFromFile`，不再依赖 `J9GetJXE`。
3. DirOpen：Z: `ive\lib` 优先改用已 stage 的 C:；C: 缺失则回退 Z:；**不再**给 `ive\lib` 造空目录。FileOpen/Entry 对 `ive\lib` 做 C:↔Z: 对开。
4. 把 midp2ams 里的 `-jcl:cldc11:nokiaextcldc` 截成 `-jcl:cldc11`。
5. 退出行增加 `[libs …]`（已加载的 j9/jcl codeseg）。

成功时控制台应出现 `[j9-nf] staged JXE`、`codeseg attached`、`fs DirOpen … prefer staged ive/lib`，且 **不再** 在 `...\0\com\symbian\...Main.class` 上 miss。

### 10.23 BUILD `687b72ab9865` 实测：JCL 已加载，但 JXESL 仍走 Z:，USER 42

```
libs j9midps60,j9_23_midp2ams.dll,j9.dll,j9vmall23.dll,j9mjit23.dll,jclcldc11_23.dll
Entry Z:\resource\ive\lib\jclCldc11\ext\0_j9_23_midp2ams_jxe.odc found=1
fs miss ...\0\com\nokia\mj\impl\vmport\VmPort.class
… J9VmPortImplCldc / Integer / UnsatisfiedLinkError / LinkageError
exit type=2 reason=42 category=USER
```

进步：JCL DLL 和 JIT 都起来了，不再卡在 `Main`/`RuntimeException`。J9 对 odc 做 **Entry 的是自己拼的 Z: 路径**，Z: 上原件存在就不会改走 C: 的 `type=JXE`。ROM odc 仍是 **JXESL**，`J9GetJXE` 按序数 Lookup(1)，但 midp2ams export 1 不是无参 getter。

末尾 `UnsatisfiedLinkError`：Java 已跑到调 native，JNI 没绑上（JXE 没从同一份 DLL 注册）。USER 42 是随后的 euser TDes/invariant。

NativeFile 钩子 `result=0x0500`：`movs r5, r0` 的小端是 `05 00`，不是 `00 05`。

**修复**：

1. midp2ams 加载时找到嵌入 ZIP（`PK\x03\x04`），把 tiny getter 的字面量改成 ZIP 地址，**export 1 改指这个 getter**（`J9GetJXE`）。
2. C: odc 保持 ROM 的 **type=JXESL**（和 native 同 DLL）；不再改写成 type=JXE。
3. Z: `ive\lib` 的 Entry/Open **只要 C: 有文件就优先 C:**。
4. NativeFile 钩子接受 `05 00`。Lookup ord≤8 打日志。

### 10.24 KERN-EXEC 3 @ `j9vmall+0x390FC`：ZIP PK 被当成 J9ROMClass

```
jxe=0x8194FF98 magic=504B0304 -> staged 'C:\jcl.jxe' (204612 bytes)
fs Open raw='...\jxe=8194FF98' resolved='C:\jcl.jxe' exist=1
Access violation reading address 0x8
  pc=0x818FAF34 (j9vmall23.dll+0x390FC)
```

`fopen("jxe=<ptr>")` 只是探测。真正当镜像用的仍是 JCL 字面量 `0x8194FF98`（嵌入 ZIP 的 `PK\x03\x04`）。`iveLoadJxe` 把 PK 当 `J9ROMClass*`，`romMethods` SRP 落到 0，`ldr r2,[r7,#8]` → KERN-EXEC 3。改 `C:\jcl.jxe` 文件内容改变不了这个指针。

JCL 里只有一处该字面量（`0x819412F4`，`ldr r2,[pc,#0x38]`）。`rom.classes`（J99J，204612 字节）完整落在 codeseg 内。把 J99J 拷到独立 RW chunk，并把该字面量改成 RW 基址。不要覆盖 XIP（会砸毁 JCL），也不要改 JCL export 1（不是 `J9GetJXE`）。

### 10.25 宿主 MIDP：对话方块 + 其它 JAR

当前 WASM 真出画走的是 **host MIDP 解释器**（`j9_host_midp.cpp`），不是 guest J9 LCDUI。

对话 `□ □ □` 常见原因：

1. `new String(byte[])` 按拉丁 1 塞进 UTF-8 缓冲 → `drawString` 画到空字形再 `fill_rect` / 字体里的 `□`
2. `System.currentTimeMillis()` 曾恒为 0 → 打字机/计时对话停在占位符

已修：

- `String([B…)` / `String([B…String enc)`：无效 UTF-8 时按 **GBK** 解码（`j9_gbk_map.inc`）
- `String([C…)`、`drawChar` / `drawChars`、`DataInputStream.read*`
- `currentTimeMillis` 用 steady_clock；`Thread.start` 会调 `Runnable.run()`
- 启动时 `j9_set_pending_midlet_class(MIDlet-1)`，主循环 `j9_host_try_attach` 不再写死 `AlpsFarm`

其它游戏（如 `生化惊悚` / `GloftMASS`）需重新安装 JAR 后强刷；若仍黑屏，看控制台 `[j9-nf] pending-midlet` / `host-try-attach` / `host-class-miss`。

### 10.26 官方 J9 LCDUI：`Canvas._create` 与 NewGlobalRef / Execute

出画必须走 **guest IBM J9 的官方 LCDUI Thumb native**（`Canvas._create` / `Toolkit.activate` / `Graphics._create` / `Buffer._flush`），不要再开 `j9_host_try_attach`。

`Canvas._create`（`0x81AEE7F8`，`push {r4-r7,lr}`）开头：

```
ldr r0, [env]          ; JNINativeInterface*
r5 = 0x380
ldr r2, [r0, #0x388]   ; slot 226 = NewGlobalRef
blx r2
… 成功后再 blx 0x81AF38A4 → CJavaEventSource::Execute (0x81A61CC4)
```

Slot 226 的 BKPT 不能种在 `walk+0x310`：pairs 从 `+0x304` 往后长，会盖掉 stub，CPU 把 pair 数据当 ARM 跑，AV 在 `walk+0x750`（读 `0x0`/`0x4`）。NewGlobalRef / NewObjectArray / CallStatic / Main cp-stub 改种到 JNI 表之后（`+0x3870` 起）。

`Execute` 是 `SendReceive([[this+8]+8], 1)`。对 `!Windowserver` 来说 function 1 = `ws_mess_shutdown`，不能真发。Toolkit._create / setCurrent 的 Execute 继续 skip。`Canvas._create` 的 callback `0x81AEE779` 目前也返回 0（本地跑 callback 会在 `[r0+0x5c]->vtable+0x68` 上 AV；真工厂要等官方 C++ peer 齐）。

Java LCDUI 对象 `+8` 是 C++ peer，不要写成 RWsSession handle。Handle 只写进 peer 内部。Dummy vtable/peer 不能放在 `walk+0x6000`（adapters 从 `+0x5000` 往后长会盖掉）。

`AlpsFarm` 不在 JCL。官方 `FindClass`（`0x818D93C8`）会拿 J9 锁并把 Main 停在 euser Wait。JAR classfile 种成合成 J9ROMClass（方法名/签名用宽松 UTF，`<init>` / `()V` 才能被 `j9_rom_method_named` 看到），`startApp` 是空 `return`，真正入口是 `AlpsFarmCanvas.<init>` + 官方 `setCurrent` / `Graphics._create`。`Graphics` 的 ROM 名 UTF 只出现在别人的 CP 里，找不到就回退到 Canvas/Object 再跑 `_create`。

合成 ROM 没有可用的 J9 RAM CP，官方解释器在 `new`/`invoke`/`getfield` 上会读 `0x52A8xxxx` 然后 AV。套件字节码改由宿主解释器按 classfile CP 跑（数组/字段/`TEXT_LOAD` 读 JAR），`Graphics.setColor` / `fillRect` / `setFont` 再踢回官方 JCL 方法（`0x8190E968` 的 `new` resolve 仍种 BKPT 作兜底）。`AlpsFarmCanvas.<init>` 末尾的 `Thread.start` 不在这里拉循环，LCDUI 链 phase 12 显式跑 `paint` + `MAIN_STATE`。

### 10.27 BUILD `7d45c32bf491`：恢复通用 host MIDP 回退

上一版把 `j9_host_try_attach()` 固定为 `false`，而官方 LCDUI 链仍把入口和 Canvas
写死成 `AlpsFarm` / `AlpsFarmCanvas`。直接后果是：牧场只能显示一次启动画面，
`GloftMASS` 等其它 `MIDlet-1` 类根本没有进入解释器。

本轮改动：

1. 主循环在 host MIDP 未激活时持续探测 `j9midps60`；确认当前 `C:\j.jar` 中能读到
   `MIDlet-1` 主类后，绑定 Windowserver surface 并以实际类名启动 host 解释器。
2. 切换套件时清空 class/object/JAR/线程缓存；class 和资源读取以当前 `C:\j.jar`
   为权威来源，避免命中上一个游戏遗留的 exploded class。
3. `run()` / `MAIN_STATE()` 到达 6ms slice 后保存 `pc + locals + operand stack`，下一帧
   从断点继续，不再每 80ms 从方法开头重跑，所以长游戏循环可以越过首屏。
4. 多个 `Thread`/`callSerially` target 使用去重后的 round-robin；音频线程不再永久
   挡住主游戏线程。
5. 补齐 Gameloft JAR 实际使用的 `wide/iinc_w`、long 常量/字段/数组/算术/比较/移位，
   以及 Wild West 使用的基础 double 运算；补充 String/StringBuffer/Hashtable 常用方法。
6. 修复中文名 storage 的历史兼容：既接受当前全下划线目录，也回退旧版 `midlet`
   目录，并可从目标目录选择实际存在的 JAR。

离线核对的目标：

- `/Users/zixing/Downloads/mu.jar`：`MIDlet-1 = AlpsFarm`，96 种实际字节码；
- `/Users/zixing/Downloads/生化惊悚完整汉化版.jar`：`MIDlet-1 = GloftMASS`，包含
  `iinc_w` 和完整 long 运算；
- `/Users/zixing/Downloads/hanhua/狂野西部汉化版.jar`：`MIDlet-1 = GloftWWGU`，
  额外使用 `i2d/d2i/dmul/dcmp*`。

WASM Release 全量链接通过，产物 build id：`7d45c32bf491`。当前 Mac 锁屏导致本轮
无法自动操作文件选择器做最终画面回归；解锁后优先按上面三个 JAR 顺序复测，并看：

```
[j9-nf] host-attach started class='...'
[j9-nf] host-thread-queue ...
[j9-nf] host-midlet '...' current=...
```

### 10.28 BUILD `e50731d98722`：FullCanvas 不是套件缺文件

`com/nokia/mid/ui/FullCanvas` 属于 Nokia UI 平台 API，正常情况下不会打包进游戏 JAR。
host MIDP 旧逻辑只把 `java/`、`javax/`、`com/sun/` 视为 JCL 类，误把 `com/nokia/`
当套件类反复从 `C:\j.jar` 查找，因此持续打印 `host-class-miss`。现为这些平台类建立
缓存的最小继承链：`FullCanvas -> Canvas -> Displayable -> Object`；实际 API 仍由
`native_invoke()` 实现。WASM Release 构建通过。

### 10.29 BUILD `a83306ecd835`：线程状态在变，但 repaint 从未调用 paint

`歪歪歪传战国风云_N70.jar` 的启动类是 `War.CatMID`，Canvas 是 `War.c`。首屏状态：

- `else = 23`、`t = 60`、图片 `/mt.png`；
- `War.c.run()` 每轮调用 `case()`，进而在 `int()` 中递减 `t`；
- 每次递减后调用 `byte()`，其内容只有 `repaint()` + `serviceRepaints()`；
- `t == 0` 后才切换 `/st.png` 并进入后续加载。

host MIDP 之前把 `repaint()` / `serviceRepaints()` 当作空 native，游戏线程虽在推进，
却从未再次调用 Canvas `paint(Graphics)`，所以屏幕永久保留第一次绘制的运营商 Logo。
现增加 repaint pending 与可恢复 paint frame：线程时间片结束后执行实际 `paint()`，超出
6ms 时保存 paint 的 `pc/locals/stack` 到下一帧继续。诊断日志为：

```
[j9-nf] host-repaint War/c graphics=... resume=...
```


