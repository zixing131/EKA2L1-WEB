# iOS 真机从不安装 HLE patch DLL（Snakes 真机黑屏无声的根因，2026-06-03 已修）

> **一句话结论**：iOS **真机**构建从来没有把 HLE patch DLL 拷进 `data/patch`，导致 `lib_manager::load_patch_libraries` 在真机上加载到**零个 patch**。缺了音频 patch → 无声；缺了 `scdv` 屏幕驱动 patch → Snakes 退回没人接的 `d_display.ldd` 直屏路径 → 活动调度器 panic → 黑屏。模拟器恰好正常，是因为它跑在 build host 上、能直接读到开发机源码树里的 patch 目录。

## 症状

- N95(RM-320, S60v3) Snakes 在**模拟器**完整可玩，在**真机**黑屏且无声。
- 真机 Snakes 启动日志在 `E:\Snake60defaults.txt` 之后唯独走：
  `LoadLogicalDevice("d_display.ldd")` → `Unimplement system call: 0x82! / 0xA!`（v93 表里未注册的 EKA2 fast-exec）→ `Active scheduler dump`（`take_on_panic`，`E32USER-CBase` 41/42/43/46 活动调度器误信号族）→ guest 死亡。
- 两次真机运行**逐行一致**——确定性，不是竞态。

## 定位过程（排除法）

| 变量 | 真机 vs 模拟器 | 结论 |
| --- | --- | --- |
| 模拟器二进制 | 同一 commit（均带诊断 build） | 相同 |
| N95 ROM `SYM.ROM` | SHA256 `0f071a15…`，60,600,320 B | **字节一致** |
| Snakes 程序 `E:\sys\bin\6r45_1b.exe` | SHA256 `7d546b…`，237,629 B | **字节一致** |
| N95 设备定义 | machine-uid `268459874` | 相同 |

数据、ROM、app 全部逐字节一致，却确定性地走不同分支 → 必是**宿主侧**差异。对启动日志做语义 diff（剔除 app-list 扫描顺序与帧日志噪声）后只剩两簇差异：

- **仅模拟器**：一批 patch 安装日志——`Can't find suitable patch DLL for map …`、`Unable to patch export N of mediaclientaudiostream_general.dll / audiooutputrouting_general.dll / ecam_general.dll due to export not exist`。
- **仅真机**：上面的 `d_display.ldd` → `0x82/0xA` → 活动调度器 panic。

真机**一条 patch 日志都没有**。直接查真机沙盒：**根本没有 `Documents/data/patch/` 目录**（而 `data/resources/` 存在）。由于 ROM 一致，若真机有尝试 patch，必然产生与模拟器**相同**的 “export not exist” 失败——既然一条都没有，说明真机**压根没加载任何 patch**。

## 根因

`src/emu/system/src/epoc.cpp` 用相对路径 `PATCH_FOLDER_PATH = ".//patch//"` 扫描 `data/patch`（cwd 由 iOS bridge `chdir` 到 `Documents/data`）。该目录由 `IosEmulator.mm` 在启动时从某个源拷入。原实现：

```objc
// 旧（坏）：只认开发机源码树绝对路径，且没有 bundle 回退
NSString *sourcePatch = [[[@(__FILE__) stringByDeletingLastPathComponent]
    stringByAppendingPathComponent:@"../../../patch"] stringByStandardizingPath];
if ([fm fileExistsAtPath:sourcePatch]) { … 拷贝 … }
```

`@(__FILE__)` 是编译期源文件绝对路径（`/Users/…/EKA2L1/src/…`），`../../../patch` → 仓库 `src/patch/`。

- **模拟器**跑在 build host 上，这个绝对路径**存在** → `fileExistsAtPath` 为真 → patch 被拷入 → 正常。
- **真机**上该路径**不存在**（那是 Mac 的路径）→ 整段 `if` 跳过 → `data/patch` 从不创建 → 零 patch。

紧邻的 shaders 拷贝**有** bundle 回退（`fileExists(bundleShaders) ? bundleShaders : sourceShaders`），所以 `data/resources` 在真机正常——patch 缺的正是同款回退。换言之 patch DLL **从未被打进 .app**，真机拷贝纯靠“伸手进开发机源码树”，只在模拟器上侥幸成立。

> 通用教训：iOS 资源 staging 若从 `__FILE__` 相对路径取源，会在**模拟器静默成功**（与 build host 同一文件系统）、在**真机静默失败**。凡要在真机用到的资源，必须打进 app bundle 并以 bundle 为首选源。

## 修复

对照 shaders 的处理，两处改动：

1. **`src/emu/ios/CMakeLists.txt`**：把 `src/patch/*/group/*.{map,_general.dll}`（仓库已跟踪的 10 个 `.map` + 8 个 `_general.dll`）以 `MACOSX_PACKAGE_LOCATION "patch"` 打进 .app 的 `patch/`。
2. **`src/emu/ios/Bridge/IosEmulator.mm`**：staging 改为优先 `NSBundle.resourcePath/patch`（真机唯一来源），源码树 `src/patch` 仅作开发机回退；bundle 是扁平布局、源码树是 `*/group/` 嵌套，`/group/` 过滤只对后者生效。

## 验证（2026-06-03，iPhone Air，真机）

- 真机重装后启动 Snakes：日志出现与模拟器**一致**的 patch 安装序列（`mediaclientaudiostream`/`audiooutputrouting`/`ecam` 的 “export not exist”、`scdv` 等）。
- `d_display.ldd` / `0x82` / `0xA` / `Active scheduler dump` **全部消失**（panic-path 行数 = 0）。
- guest 转而走与模拟器**相同的成功路径**：`Snake60defaults.txt` → msv → etel `Opening DefaultPhone` → AKNCAP/窗口服务器 UI 合成；无 panic / AV / halt。
- 音频 patch（`mediaclientaudiostream`/`audiooutputrouting`）已加载，声音恢复。
- **用户肉眼确认：真机 Snakes 声音与画面均正常、可正常游玩。** 黑屏无声问题已解决。

## 遗留

- 真机上仍有**性能问题（卡顿）**，与本修复无关，留待后续单独排查（2026-06-04+）。

## 复现/工具备忘

- 真机装机：`EKA2L1_IOS_DEVELOPMENT_TEAM=L6JP27B8YR EKA2L1_IOS_DEVICE=77611A2B-2A02-51FA-BAFC-2104F1D8011A scripts/build_ios.sh install`
- 直接启 Snakes：`xcrun devicectl device process launch --device <UDID> --terminate-existing com.eka2l1.emulator -LaunchAppUID 0x2000730F`（真机锁屏会报 `Locked`，需先解锁）
- 拉日志：`xcrun devicectl device copy from --device <UDID> --domain-type appDataContainer --domain-identifier com.eka2l1.emulator --source Documents/data/EKA2L1.log --destination <out>`
- 查真机 patch 是否到位：`xcrun devicectl device info files --device <UDID> --domain-type appDataContainer --domain-identifier com.eka2l1.emulator | grep data/patch`
- 真机 UDID：`77611A2B-2A02-51FA-BAFC-2104F1D8011A`（iPhone Air）；模拟器：`408537AD-C7C8-490D-AE57-AB4817F18E15`。

## 关联

- 模拟器侧 N95 Snakes 的 stray-signal 修复（独立、仍成立）见 [`ios-snakes-stray-signal.md`](./ios-snakes-stray-signal.md)。该文档「真机回归」节早先把真机黑屏误归为同一 kernel 竞态，已更正——真机根因是本文所述的缺 patch。
- N97(S60v5) 真机另有“有声音没画面”的 UI 合成缺口，与本问题无关，见 [`s60v5-avkon-fep-pti.md`](./s60v5-avkon-fep-pti.md)。
