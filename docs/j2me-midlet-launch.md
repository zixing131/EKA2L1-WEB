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


