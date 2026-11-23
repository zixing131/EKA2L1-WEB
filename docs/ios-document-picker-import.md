# UIDocumentPicker 文件导入

> 来源：阶段 3.5（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：🟡 主链路完成，少量 follow-up 待办。
>
> **一句话结论**：首页重设计为以设备为中心（无设备显示空态 CTA、有设备进 app 列表），新增 `ImportDeviceView` 走真实 ROM/RPKG 安装、`+` 走 `.fileImporter` 装 SIS；安全作用域 URL 必须先拷暂存。剩余 ZIP 解包 / Share extension 等推迟。

- ✅ Info.plist：新增 `CFBundleDocumentTypes` 三条（SIS/SISX viewer-owner、ROM zip viewer-alt、TTF/OTF viewer-alt）+ `UTExportedTypeDeclarations`（`com.eka2l1.sis` / `com.eka2l1.sisx` conform 到 `public.data` + `.sis` / `.sisx` extension tag）。`UIFileSharingEnabled` 与 `LSSupportsOpeningDocumentsInPlace` 已经在阶段 0 打开，保持不变。
- ✅ **首页重设计（2026-05-29）**：`ContentView` 从原"ROM 列表 → AppListView → Emulator"三屏改为以设备为中心。无设备时显示 `ContentUnavailableView`（标题 No device installed + Android `no_device_installed` 文案 + "Install device" CTA）；有设备时直接进 app 列表，标题为设备名（`EKA2L1DeviceItem.displayName`，取 model 回落 firmcode），右上 `⋯` Menu 含 Settings / Devices（二级菜单切换设备，当前项带勾选）/ Install device / Diagnostics，左侧 `+` 走 `.fileImporter` 装 SIS 到当前设备。`AppListView` 整个删除。
- ✅ **设备导入页 `ImportDeviceView`**（Form）：第一行选 ROM 文件、第二行选 RPKG 文件（可选），section footer 是 Android `install_device_note_may_need_rpkg` + `recommended_devices_to_install` 文案，底部 Install CTA。选文件时在安全作用域内把文件暂存到 `Documents/import_tmp/`（路径在 picker 关闭后失效，必须先拷），Install 时在后台队列调 `EKA2L1Bridge.installDevice(romPath:rpkgPath:)`（见 3.4），成功后清 `import_tmp` 并 boot 新设备。
- ⚠️ **fileImporter bug 修复**：同一视图上叠加两个 `.fileImporter`（ROM + RPKG）会被 SwiftUI 丢弃其一导致点击无反应。改为单个 `.fileImporter` + `pickTarget` 枚举多路复用（`showingImporter` bool 驱动展示，`pickTarget` 在 onCompletion 里读取，不在 binding setter 里清空以免竞态读到 nil）。
- ✅ `src/emu/ios/App/ImportRouter.swift`：仍负责 SIS 的安全作用域 URL pair（`startAccessingSecurityScopedResource` / `stopAccessing...`）+ 按扩展名分发（`.sis/.sisx → Documents/sis/`、`.ttf/.otf → Documents/data/fonts/`）。`.zip` / `*.rom` 分支保留但 SIS 导入入口的 UTI 过滤不会再走到（ROM 现在走 `ImportDeviceView` 的真实安装路径，不再落 `Documents/roms/`）。
- ✅ Info.plist：`CFBundleDocumentTypes`（SIS/SISX viewer-owner、ROM zip、TTF/OTF）+ `UTExportedTypeDeclarations`（`com.eka2l1.sis` / `com.eka2l1.sisx`）；`UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace`（阶段 0 打开），所以 app 的 Documents 出现在 Files「浏览 → 我的 iPhone → EKA2L1」下，是导入 ROM/RPKG 的取文件来源之一。
- ✅ 端到端验证（iPhone 16 Pro / iOS 26.5 模拟器，2026-05-29）：把工作区 `SYM.rom` + `rm-320.rpkg` 拷进 app Documents → 文件选择器浏览 EKA2L1 文件夹选中两文件 → Install → 安装成功进 applist（详见 3.4）。
- 🟡 剩余 follow-up：①ZIP unzip 走 miniz 暂未做（现在 ROM 走真实 install_rom/rpkg，ZIP-bundle 导入需求降级）；②"Share to EKA2L1" extension（外部应用 Share 菜单直接送进来）推迟到 stage 3 收尾或更后；③`scripts/seed_ios_simulator_documents.sh` 继续保留为开发期复跑捷径，不进 release 路径。
