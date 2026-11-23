# 相机取景下 Avkon 菜单被取景覆盖/闪烁

## 现象

iPhone Air 真机运行 Nokia 5320d-1（rm-409）内置 Camera（UID `0x101F857A`），取景框正常显示。按左软键打开 Options 菜单时，菜单窗口只显示一帧就立刻被取景画面覆盖回去，无法正常操作菜单。

模拟器无相机设备（count=0），此路径只能在真机复现。

## 定位过程（真机 window server 埋点）

先后排除了几条错误方向：

- **不是 DirectScreenAccess (DSA) 问题**：相机取景根本没走 DSA（DSA 埋点零触发）。取景是以普通合成窗口方式（bitmap-viewfinder，经 `ecam_receive_image` 送帧）到屏幕。
- **不是可见区域/z-order 计算错误**：菜单稳定打开时，`recalculate_visible_regions` 算出的可见区域是**正确**的——菜单面板窗口在最上、取景窗口被裁成菜单四周的边框（`vis_rects=3`）。
- **一度被误导为按键重复**：诊断期间用户点到右软键（`0xA5`）会关菜单，造成"一开一关"的假象；干净单点左软键（`0xA4`）后菜单能稳定打开，排除输入层 bug。

真正的病理来自逐帧绘制路径（`redraw_msg_canvas::draw`）：

- 相机取景窗口每帧走的是**客户端（局部）重绘**（`FLAG_CLIENT_REDRAW_PENDING`，`server_pending=false`）。
- 客户端重绘**不清屏、也不重新合成上层窗口**，只把当前窗口的客户端绘制命令裁剪到自身 `visible_region` 后合并进屏幕纹理。
- 菜单面板窗口（redraw 窗口）的内容只存在于它的 server 段（`server_segs`），只有**服务端整屏重绘**时才会被重画；平时的客户端重绘分支不画它。
- 取景窗口的 `visible_region` 是多矩形（菜单四周的边框），走 stencil 版 `graphics_driver_clip_region`；被合并进来的 guest 取景绘制命令并未可靠地遵守该多矩形裁剪，逐帧渗进菜单区域，而菜单区域在客户端重绘里又永远不会被重画覆盖回来 → 取景盖住菜单、并随偶发的服务端整屏重绘而闪。

## 修复

`canvas_base::try_update`（`src/emu/services/src/window/classes/winuser.cpp`）：当一个窗口即将做客户端重绘、而它**被部分遮挡**（`visible_region` 非空且不等于自身 bounding rect / shape region）时，额外置上 `FLAG_SERVER_REDRAW_PENDING`，升级为一次服务端整屏合成。

服务端整屏重绘会清色缓冲并按后到前顺序重画所有可见窗口，遮挡它的上层窗口（菜单）随之重新画在最上面，覆盖掉下层取景任何越界像素。仅在"重绘窗口本身被遮挡"这一必要场景触发，绝大多数不可见窗口在 `can_be_physically_seen()` 提前返回，开销可控。属共享 window server 修复，不含相机或 iOS 特判。

## 验证

- iPhone Air + rm-409：干净单点左软键后 Options 菜单稳定显示在取景之上，不再被盖/闪（用户确认"正常了"）。
- Release 模拟器回归：Final Battle + Calculator **8/8** PASS；触屏套件 Angry Birds（X7/rm-707）**5/5** PASS（覆盖 redraw/合成路径无退化）。

诊断期间加入的 `[MENUDBG]` window server 埋点均已删除。
