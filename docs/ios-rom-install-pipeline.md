# 真正的 ROM 安装流程（取代 symlink / hardlink graft）

> 来源：阶段 3.4（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已完成。
>
> **一句话结论**：废弃早期 hardlink graft + 手写 `devices.yml` 的权宜方案，改走与 Android / macOS 完全一致的真实安装管线（`install_device` / `install_rom` / `install_rpkg`），并把设备选择持久化到 `conf.device` 下次自动 boot。

- ⚠️ **hardlink graft 已废弃**：早期（3.4 第一版）`mountRomNamed:` 用 `NSFileManager.linkItemAtPath:toPath:` 把 `roms/<rom>/data/drives/z` + `data/roms/<firm>/SYM.ROM` 硬链进 sandbox，并手写 `devices.yml`。这要求 ROM 文件夹已经是 desktop 预装好的 device 树，是个权宜方案。2026-05-29 整段移除，改走与 Android / macOS 完全一致的真实安装管线。
- ✅ 真实安装 API：Obj-C facade 把 `availableRoms` / `mountRomNamed:` 换成 `installedDevices` / `currentDeviceIndex` / `installDeviceWithRomPath:rpkgPath:` / `bootDeviceAtIndex:`。`installDeviceWithRomPath:rpkgPath:` 镜像 `launcher::install_device`：`loader::should_install_requires_additional_rpkg(rom)` 为真 → `loader::install_rpkg(dvc, rpkg, drives/z, ...)` + 把 ROM 拷成 `roms/<firm>/SYM.ROM`；为假 → `loader::install_rom(dvc, rom, roms/, drives/z, ...)`（install_rom 内部已经把 ROM 拷到 `roms/<firm>/SYM.ROM`）。完成后 `dvc->save_devices()` 落 `devices.yml`，**不再手写**。
- ✅ `bootDeviceAtIndex:` 抽出原 mount 尾段（重建 `system` → `startup` → `set_device(idx)` → mount c/d/e/z → 绑 graphics → 注册 per-screen redraw callback），并把选中的 index 写回 `conf.device` + `serialize()`，下次启动 `startWithDocumentsPath:` 末尾自动 boot 上次设备。
- ✅ 安装结果走新枚举 `EKA2L1InstallResult`（1:1 镜像 `device_installation_error` + iOS 专属 `NeedRpkg`），前端用从 Android `strings.xml` 移植到 `Localizable.xcstrings` 的文案（`import.error.rpkgCorrupt` / `import.error.alreadyExists` / ...）。
- ✅ 并发：设备安装 / 切换会重建 `symsys` 或改 `device_manager`，新增 `loop_mutex`（os_thread 每个 `symsys->loop()` tick 持有），install/boot 先翻 `mounted=false` 再抢锁排空在途 tick；安装失败时把 `mounted` 还原回原值，让原本运行的设备继续跑。重操作经 `nonisolated static` 桥接入口在后台队列跑，UI 转 spinner。
- ✅ 端到端验证（iPhone 16 Pro / iOS 26.5 模拟器，2026-05-29）：空 sandbox → 空态 `ContentUnavailableView` → Install device → 文件选择器选 `SYM.rom`(60.6MB) + `rm-320.rpkg`(98.7MB) → "Processing…" → `install_rpkg` 成功 → 自动 boot 新设备 → 标题 "Nokia N95 (01.01)"、applist 62 app、图标正常。
- 备注：裸 ROM（无 device tree 的 .rom）现在天然由 `install_rom` 的 ROM-dump 解析覆盖，不再需要单独的"自动生成 minimal device tree"路径。
