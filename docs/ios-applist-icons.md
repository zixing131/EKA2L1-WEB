# AppList 图标（SVG / MIF 解码）

> 来源：阶段 3.6（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已完成。
>
> **一句话结论**：`iconPNGDataForUID:sizePx:` 按 Android 同款顺序解 `.mif`(lunasvg) / `.mbm` / fallback `get_icon`，缩放成方形 PNG 给 SwiftUI 懒加载；附带修了空格 caption 触发的 `pystr` 空串崩溃。

- ✅ `EKA2L1Emulator` 新增 `iconPNGDataForUID:sizePx:`，按 Android 同款顺序尝试解码：①`.mif` → `loader::mif_file` + `convert_svgb_to_svg` / `convert_nvg_to_svg` debinarize 到 `Library/Caches/icons/<firmware-code>/debinarized_<UID>.svg`（带 mtime cache；firmware code 隔离同一 UID 在不同 ROM 中的资源，UID 避免同名应用互相覆盖）→ `lunasvg::Document` 光栅化到内置 width/height；②`.mbm` → `loader::mbm_file` + `epoc::convert_to_rgba8888(fbsserv, ..., 0, dst)`；③其它/失败 fallback → `alserv->get_icon(*reg, 0)` 取 `bitwise_bitmap` pair + `convert_to_rgba8888(fbsserv, bitmap, dst)`。
- ✅ 解码出的 RGBA 走 `CGBitmapContext + CGContextDrawImage` 缩放到调用方请求的方形尺寸（默认 72px，给 SwiftUI 一份 stable 画布），再 `UIImagePNGRepresentation` 编 PNG 返回 `NSData`。
- ✅ SwiftUI `AppRow` 在 `.onAppear` 把解码 dispatch 到 `DispatchQueue.global(qos: .userInitiated)`，回主线程赋 `@State`；解码失败 fallback 到 `Image(systemName: "app.dashed")` 占位。CMake 给 iOS target 加 `epocloader` / `lunasvg` 链接依赖。
- ✅ xcodebuildmcp 验证（iPhone 16 Pro 模拟器）：mount N95 → AppList 出 Help (?)、Messaging (信封)、Voice recorder (麦克风)、Settings (扳手)、Call mailbox、Profiles、Calendar (30)、Calculator 等真实 S60 图标。截屏 `docs/screenshots/ios-stage3/3.6-icons/applist-with-real-icons.jpg`。
- ✅ 2026-05-24 追加稳定性修复：N95 applist 里存在 caption 为空格的条目（`uid=0x101F4CD2`），滚动触发图标懒加载时 MIF cache name sanitize 成空串，`pystr::strip/rstrip` 在空 string 上调用 `back()` 被 libc++ hardening abort。修法：`pystr::{lstrip,rstrip}` 空串 guard；当时为空文件名增加 UID fallback，现已统一按 UID 命名；iOS 图标解码入口加 mutex 串行化 applist/fbs/io 访问。验证：iPhone 16 Pro 模拟器连续滚动到中后段和底部，无新 `EKA2L1-*.ips`。
- 字体缺失时 SVG 文本会失败 → 3.12 字体引导覆盖。
