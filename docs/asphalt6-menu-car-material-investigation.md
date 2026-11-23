# Asphalt 6 主菜单车身材质调查

状态：**已解决（2026-07-14）。**

环境：iOS Simulator、X7 / rm-707、Asphalt 6 UID `0x2003B2CC`。Android 对照截图（`screenshots/screenshot.png`）确认 Mini 应为不透明、饱和的蓝色。

## 根因

`src/emu/drivers/src/graphics/backend/graphics_driver_shared.cpp` 中
`instantiate_bitmap_depth_stencil_texture()` 在 `EKA2L1_PLATFORM(IOS)` 下直接返回
`nullptr`。这是早前为绕过 iOS GLES 拒绝 depth24-stencil8 **纹理**附件
（`GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT`）加的 workaround，当时的假设是"window-server
位图只是 2D 颜色目标"。

但 **EGL window surface 同样以 bitmap FBO 作为后备**（`egl_surface::scale_and_bind` →
`bind_bitmap`），于是 iOS 上所有 GLES 游戏的窗口表面都没有深度缓冲。按 GL 规范，无深度
附件时深度测试恒通过——整个场景退化为画序（painter's order）。

Asphalt 6 主菜单的渲染顺序是：不透明环境 → 不透明车身 → 大量 alpha/additive 混合层
（地板光泽、展厅光柱、贴花）。车身（`TEXTURED + MULTITEXTURED` variant，index count
4449/1194）**被正确画出（饱和蓝色）**，随后被本应被车身深度值挡掉的混合地板/光泽 pass
逐层覆盖，最终只剩轮子、玻璃和暗色残影。Android/桌面没有这个 ifdef，深度纹理附件可用，
因此正常。

## 修复

iOS 上改用 **renderbuffer** 做位图 FBO 的 packed depth-stencil（GLES 对
`GL_DEPTH24_STENCIL8` renderbuffer 的支持是普遍的，当年不完整的只是纹理附件路径）：

- `bitmap::ds_tex` 类型从 `texture_ptr` 放宽为 `std::unique_ptr<drawable>`
  （`make_framebuffer` 本就接受 `drawable*`，`fb_ogl` 对 renderbuffer 走
  `glFramebufferRenderbuffer` + 合并 `GL_DEPTH_STENCIL_ATTACHMENT`）。
- `instantiate_bitmap_depth_stencil_texture()` 的 iOS 分支创建
  `make_renderbuffer` + `depth24_stencil8`；其它平台保持原深度纹理路径不变。

修复后主菜单 Mini 为不透明饱和蓝色（与 Android 一致），且 FPS 由 16 升至 21（减少了
无效覆绘）。验证时 `glGetFramebufferAttachmentParameteriv(GL_DEPTH_ATTACHMENT)` 从
`GL_NONE` 变为 `GL_RENDERBUFFER`。

## 已被证伪的假设（不要再走）

| 假设 | 证伪证据 |
| --- | --- |
| attribute stream/location 错位 | 对 1716/4449/246 等 draw 逐一核对 guest 喂流与 host link 布局（含 ubyte normalized Color0、常量 attribute 仿真），全部一致 |
| 纹理 V 翻转 | 把 draw 的 UV 三角形画到实际上传的 atlas 上，与轮拱/格栅/面板特征精确对齐；车漆 atlas（client 93，游戏 CPU 端染色合成后一次 `glTexImage2D` 上传）内容完好 |
| `count==1716` 是主车漆 | 它是深色内衬/格栅/下摆 trim，输出黑灰是**正确**的 |
| GL/EGL 字符串特征检测 | 换成真机 SGX530 全套字符串无变化 |
| dyncom CPU 误算 | dynarmic JIT 下同样复现 |
| overlay 自身 alpha/LOD/cull/depth state | 各 draw 的 host GL 状态全部正确 |

## 本次定位的关键手法

1. host `draw_indexed` 一次性 per-(program,count) 状态 dump（纹理绑定、blend、attrs、sampler uniforms）。
2. HLE `glDrawElements` 侧按 (guest prog, count) 记录喂流 + 常量 attribute + 纹理尺寸，与 host 侧互相印证。
3. 对候选 draw 做 pre/post framebuffer dump → 证明车身画出来了。
4. **对车身 draw 之后的 30 个 draw 逐个截帧**，用蓝色像素计数追踪 → 找到覆盖者（混合地板/光柱 pass）。
5. 覆盖者深度测试开启却能盖住更近的车身 → 查 `GL_DEPTH_ATTACHMENT` → `GL_NONE`，定位到位图 FBO 无深度附件。

## 验证

- Asphalt 6 主菜单车身恢复蓝色（模拟器截图确认）。
- Release 标准回归（Final Battle + Calculator）与 Asphalt 专用回归见 `IOS_PORTING_TASKS.md` 当次记录。
- 所有临时诊断（host draw dump、HLE 追踪、序列截帧、guest 网格 dump）已从源码移除；游戏容器内的宽松 shader（`profilecommon_emul_fs.glsl`）已恢复原始内容。
