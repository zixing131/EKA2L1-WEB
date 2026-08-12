# J2ME MIDlet 启动链路（S60v3 / WASM）

本文档记录 EKA2L1 WASM 构建中，S60v3 ROM 下 J2ME MIDlet 的安装、注册、启动流程，
以及为解决「跨页面注册丢失」问题所做的全部改动。

---

## 一、背景：S60v3 Java 运行时架构

一个 Java-capable 的 S60v3 ROM 通常包含以下组件：

| 组件 | 路径 | 作用 |
|------|------|------|
| MIDP2 静默安装器 | `Z:\sys\bin\midp2silentmidletinstall.exe` | 从 JAD/JAR 把 MIDlet 注册进 AppArc。需以 `systemamscore.exe` 为 creator 调用（校验 SID 0x10203636） |
| AMS 核心 | `Z:\sys\bin\systemamscore.exe` | Java 生命周期/安全/注册表服务器，静默安装器的父进程 |
| MIDP2 KVM | `Z:\sys\bin\midp2runtimev2.dll` | 实际执行 Java 字节码的虚拟机（KVM）。**缺失则无法运行任何 Java** |
| MIDP2 启动器 | `Z:\sys\bin\midp2midletlauncher.exe` | 独立启动器 exe（部分 ROM 有，用于直接启动 MIDlet） |
| 文件识别器 | `Z:\sys\bin\StubMIDP2RecogExe.exe` | **不是启动器**。AppArc 文件识别器，以 opaque data 调用，直接 spawn 无效 |

### 启动路径（两条，互为补充）

1. **AppArc 非原生路径**（首选）：MIDlet 注册到 AppArc 后，类型为
   `0x10210E26`/`0xB031C52A`（非原生 Java），由 `alserv->launch_app()` 启动。
   该路径需要 `midp2runtimev2.dll`。
2. **独立启动器路径**（回退）：直接 spawn `midp2midletlauncher.exe`，以 JAD 路径为命令行参数。
   需要该 exe 存在于 ROM。

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
  └─ Strategy 1: AppArc 非原生启动
       ├─ 扫描 AppArc 找类型 0x10210E26/0xB031C52A 且 name 匹配的注册项
       ├─ 命中 → alserv->launch_app() → 成功返回 0
       └─ 未命中 → reregister_midlet() → 返回 -100 (JS 重试)
  └─ Strategy 2: 独立启动器 spawn
       └─ j2me::launch() → 成功返回 0
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
     ├─ Strategy 1: 扫描 AppArc
     │   ├─ 命中 → launch_app → 成功 (0)
     │   └─ 未命中 → reregister_midlet → 返回 -100
     │                              │
     ▼                              ▼
launch === -100?              reregister_midlet:
     │ 是                       ├─ restage JAR/JAD
     ▼                          ├─ spawn systemamscore
显示「正在注册…」遮罩             └─ spawn silent installer
     │                              └─ 异步: rescan + status=2
     ▼
轮询 midletInstallStatus() ──────────── 安装器退出
     │ status==2
     ▼
重新 launchMidlet(uid)
     │
     ▼
AppArc 命中 → launch_app → 成功 (0) → 运行
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
| 启动后无画面 2 分钟 | KVM 启动慢或失败 | 查控制台 panic 日志 |
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
# 用带 COOP/COEP 头的本地服务器调试 WASM（SharedArrayBuffer 必需）
python3 scripts/serve_wasm.py 8080 build_wasm_release/bin
# 然后访问 http://localhost:8080/
```


