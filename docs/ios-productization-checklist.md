# iOS 产品化 Checklist（App Store 上架 + 面向大众）

> 评估日期：2026-07-05（基于 iPhone 16 Pro / iOS 26.5 模拟器实测 + 代码审视）。
> 结论：模拟核心链路（装机 → 装 SIS → 进游戏 → 输入 → 音频 → 振动）已打通。
> 2026-07-05 已补齐首批产品外壳：图标/启动屏/版本/隐私清单、首次使用引导、
> 英文 String Catalog、设置即时生效与会话内常用操作。
> 状态：`- [ ]` 未做 / `- [x]` 完成。与 [`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md) 阶段任务互补，
> 本文只管"离上架还差什么"，不重复移植期的技术任务拆解。

---

## P0 — 上架硬门槛（缺一项就无法提交 / 无法过审）

### 提交材料
- [x] App 图标：参考 `muhannad-ios/master` 的 Icon Composer 素材风格，保留
      `Resources/AppIcon.icon` 为 source of truth；CI 使用 macOS 26 / Xcode 26，构建时直接交给
      `actool` 编译出 Assets.car / PNG fallback。
- [x] 启动屏：`UILaunchScreen` 使用系统推荐的空字典配置，避免维护额外启动屏图片资源。
- [x] 版本号与 bundle 元数据：`CFBundleShortVersionString=26.7.0`，`CFBundleVersion=260700`。
      规则：`YY.M.patch`，例如 2026 年 7 月第 1 个版本为 `26.7.0`。
- [x] PrivacyInfo.xcprivacy 隐私清单：声明不跟踪、不采集数据；盘点并声明 required-reason API：
      UserDefaults、文件时间戳、磁盘空间、系统启动时间。后续新增 SDK/静态库时必须复扫。
- [ ] App Store 页面素材：截图、描述、关键词（注意商标红线，见下）

### 合规红线
- [ ] 发布前复核上架版**无 JIT**：本轮已把用户可见 CPU backend 选择器移除，配置保存时强制 dyncom；
      阶段 4 dynarmic / MAP_JIT 仍不得进入 App Store 默认路径。
- [ ] 发布前复核固件/ROM 完全用户自备：App 内文案、帮助链接不得指向 ROM 下载源；
      设备导入页"推荐机型"提示文案按此尺度复审。
- [ ] 发布前复核 Nokia / Symbian 商标使用（App 名称、截图、描述中避免直接使用）。

### 部署目标与稳定性
- [x] 部署目标决策：`EKA2L1_IOS_DEPLOYMENT_TARGET=16.0`；当前 simulator 产物
      `MinimumOSVersion=16.0`。
- [x] guest 崩溃不带崩宿主：当前 guest fatal 已通过 `launch_app` logon 回调转成
      EmulatorView alert 后关闭会话；宿主不应因 guest panic 直接闪退。发布前仍需随回归脚本复核。
- [x] Snakes 残留 stray-signal panic 收尾：已按当前状态更新为已解决路径；`patch/` bundle staging
      和调度修复覆盖 Snakes 误信号/黑屏的已知根因，回归脚本继续把 Snakes/Final Battle/Calculator
      类路径作为守门。
- [x] 设置页 CPU backend 移除 dynarmic 选项：用户设置页不再暴露 CPU backend / JIT 占位，
      保存时固定 dyncom。
- [ ] 发版门槛固化：`scripts/ios_regression_test.sh` 全绿才可出包

## P1 — 大众可用性（不做则普通用户走不通第一步）

### 首次使用引导（最大 UX 缺口）
- [x] Onboarding 流程：首次启动展示 3 页引导（安装自备 device ROM/RPKG、添加 SIS/SISX/N-Gage、
      BYO content 合法性提示），并提供菜单入口可重新打开。
- [ ] ZIP 固件包一键导入（Info.plist 已声明 zip UTI，解包逻辑为 3.5 遗留待办）

### 本地化
- [x] 接 String Catalog：新增 `Localizable.xcstrings`，稳定 dot key，当前只维护英文；CMake 将
      `.xcstrings` 作为 Xcode target resource 接入，并标记 `text.json.xcstrings` 交给 Xcode 处理。
- [x] 首版只支持英文：符合本轮要求；中文和其他语言延后到正式市场素材准备阶段。

### 设置页产品化
- [x] 去掉 "Save" 按钮，改即时生效 + 自动持久化（iOS 惯例）
- [x] 3.10 遗留"多数设置需重启生效"：前端设置改为变更即保存/应用；清除数据等破坏性操作明确提示重启。
- [x] 移除/隐藏开发者项：CPU backend 选择器、"JIT Stage 4" 占位、log filter、
      Diagnostics smoke test（可移到隐藏开发者页）
- [x] 加用户项：日志导出/分享（反馈渠道）、清除数据、存储占用查看

### 模拟中的会话体验
- [x] `isIdleTimerDisabled`：游戏中防自动锁屏，离开会话恢复原值。
- [x] AVAudioSession 中断恢复（来电、闹钟）：中断开始 pause，允许恢复时 resume。
- [x] 游戏内菜单：新增退出/重启当前 guest app 的明确流程；返回路径继续保留。
- [x] 截图保存到相册：新增会话菜单保存截图，并补 `NSPhotoLibraryAddUsageDescription`。
- [x] 应用列表行的 `uid=0x...` 调试信息去掉；UID 仍保留在模型、启动参数和调试路径中。

### 文本输入补全
- [ ] UIAlertController 输入框真实场景验证（Notes 等，3.11 遗留）
- [ ] QWERTY overlay 或系统键盘桥接（短信/备忘录类字母输入，数字九宫格覆盖不了）

### 虚拟键盘完善
- [ ] 按键透明度/大小调节、位置拖拽
- [ ] 横屏布局适配
- [ ] 每游戏记住布局偏好

## P2 — 完整度与打磨

- [ ] 音频收尾：MIDI bank 验证（铃声/游戏配乐大量用 MIDI）、FFmpeg 真机路径、
      实际听感验证（3.7 / 3.7.1 遗留）
- [ ] 字体导入引导（3.12 未开始）：缺字体检测 + 引导横幅
- [ ] iPad 适配：键盘 overlay 尺寸、分屏、Stage Manager（首版可只勾 iPhone）
- [ ] Metal via ANGLE（GLES 已弃用的长期技术债 + 真机性能钥匙；
      见 `ios_metal_angle_plan.md`，可排上架后第一个大版本）
- [ ] 每应用配置：分辨率/缩放/滤镜/键盘布局 per-app 记忆
- [ ] 无障碍：虚拟键盘 accessibility label、Dynamic Type 检查
- [ ] 崩溃监控：至少接 MetricKit

## P3 — 发布工程（阶段 4 的非 JIT 部分）

- [ ] CI + 归档流水线：自动化 archive + 签名 + 上传 TestFlight（现状仅本地 `build_ios.sh`）
- [ ] TestFlight 公测：收集真机机型（不同 SoC / iOS 版本）崩溃数据后再正式上架
- [ ] 回归脚本扩容：每个支持的 ROM 世代（S60v1/v2/v3/v5、N-Gage）至少一个代表 app

---

## 已确认无需做 / 已具备

- JIT（dynarmic）：App Store 版本**不需要**——解释器路线即合规路线
- 后台自动暂停：已有（scenePhase → `EKA2L1Bridge.pause()`）
- 游戏手柄：GCController 接入已有（连接/断开/extendedGamepad 映射）
- 外接键盘：UIPress → raw key 映射已有（含软键/拨号/挂断）
- 应用图标解码（MIF/MBM/SVGB）、SIS/N-Gage 安装、设备多固件切换、振动：已完成
