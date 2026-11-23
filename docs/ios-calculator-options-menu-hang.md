# Calculator「Options」软键卡死 / AVKON 菜单不显示

## 症状

iOS 上启动 Calculator（`0x10005902`），点左软键打开 **Options** 后 guest 应用
**卡死**：软键标签（Options/Exit）消失、任何按键无响应、需重启 app。

## 第一层根因（已修复）：未完成的 window-server IPC 导致死锁

按 Options 时 AVKON 会为菜单/popup 的背景退避（fader）创建一个 **backed-up
window**（`RBackedUpWindow` → `bitmap_backed_canvas`），并对它同步发送窗口
opcode `EWsWinOpEnableBackup (0x57)`。

`bitmap_backed_canvas::execute_command`（`winuser.cpp`）的 `default` 分支只
`LOG_ERROR` 而 **没有调用 `ctx.complete()`**：

```
E [Service.Window]: Unimplemented bitmap backed canavas opcode 0x57!
```

window-tree 节点操作是**同步** `SendReceive`，服务端不 complete，guest 调用方
就永久阻塞在请求信号量上 → active scheduler 没有可运行线程 → 整个 UI 冻结。
`sample` 显示 CPU 线程停在 `thread_scheduler::switch_context → event::wait()`，
是 idle wait 背后挂着一个永不完成的 IPC（不是忙等）。

兄弟类 `redraw_msg_canvas::execute_command` 的 `default` 是正确的——它会
`ctx.complete(epoc::error_none)`。

**修复**（`winuser.cpp`）：给 `bitmap_backed_canvas::execute_command` 的
`default` 补 `ctx.complete()`，并显式把 `EWsWinOpEnableBackup` 当 no-op 成功
处理（该窗口本就是 bitmap-backed，启用 backup 无需额外动作）。任何未实现的
同步 opcode 不再把 guest 拖死。

**验证**：修复后按 Options 不再出现 `0x57` 报错、log 不冻结、guest CPU 正常
回到 idle（非死锁忙等），可继续运行——底层死锁消除。

## 第二层（独立问题，未解决）：菜单不可见

修复死锁后，菜单**逻辑上打开**（进入 modal、吞掉数字键），但**屏幕上画不出
菜单**。经约 18 次插桩重现，定位如下（根因在 guest 二进制，需逆向）：

**确定的事实**

- 按 Options 时窗口服务只创建出：menu-bar 容器窗口（如 win=24，320×240，
  redraw）、CBA 软键条（如 win=25/26）、popup fader（如 win=27，backed-up）。
  其中 **menu-bar 容器窗口创建后从未收到任何窗口操作**（无 SetExtent/
  Activate/SetVisible/绘图），始终 `is_visible()==false`、`visible_region`
  为空，因此不渲染。**真正带菜单项的 menu pane 窗口根本没有被创建**——菜单
  构造在创建/填充它之前就中止了。
- 中止点：菜单构造期间 `!AknIconServer` 线程渲染菜单图标时 guest 主动
  `User::Leave(KErrNotSupported, -5)`（被 TRAP 捕获）。涉及加载 `avkon2.mif`
  与 NVG 图标渲染 ECom 插件 `102827CF.dll`（文件均已 staged、DLL 加载成功）。
  注意 EKA2L1 的 NVG→SVG 转换（`loader::convert_n_to_svg`）只用于 Qt 前端
  app-list 图标，**guest 内部 AknIconServer 的图标渲染走 guest 自己的代码**。

**已逐一证伪的 emulator 侧假设**（均非根因）

- screen device `set_screen_mode_and_rotation` 返回 not_supported：菜单期间
  从未触发（0 次 “mode not found”）。
- 某个 window-server IPC 向 caller 返回 `-5`：在 `ipc_context::complete`
  拦截 `res==-5`，菜单期间**无任何命中**。
- `AknIconServer` 向 window server 发不支持的命令：在 `execute_commands` 按
  caller 线程过滤，菜单期间 AknIconServer **未发任何 window-server 命令**
  （leave 时栈上的 `ws32.dll` 帧是其启动期的残留）。
- `EWsWinOpStoreDrawCommands` 被 stub：菜单路径有时根本不触发它，故非主因。

**结论（已 dump+反汇编逐级定位到精确 ROM 指令）**：`KErrNotSupported` 由
`!AknIconServer` 渲染菜单图标时主动 `User::Leave(KErrNotSupported)`，源头是一个
**只支持 32bpp 显示模式的 draw-device 工厂**：

```arm
; ROM 图形模块 (graphics draw-device factory), ARM 代码
0x..E7A8: cmp r0, #0xB     ; r0 == EColor16MU (32bpp)?
0x..E7B0: beq <handle>
0x..E7B4: cmp r0, #0xC     ; r0 == EColor16MA (32bpp)?
0x..E7B8: bne 0x..E80C     ; 其它模式 → leave
0x..E80C: mvn r0, #4       ; r0 = -5 (KErrNotSupported)
0x..E810: bl  User::Leave  ; LEAVE
```

工厂只接受 `EColor16MU(0xB)` / `EColor16MA(0xC)`（两个 32bpp 模式），其余一律
leave。NVG 矢量图标渲染需要带 alpha 的 32bpp 渲染面；该工厂为 NVG 内部渲染面
创建 draw device，但拿到的 display mode 不是 32bpp。

**定位过程的可靠手法**（栈扫描不可靠，会拾取 RHeap 陈旧帧）：`leave_start`
exec 是 `User::Leave` 经 `svc #0xDE` 直接触发（exec stub 不 push），故 leave 时
`User::Leave` 帧即当前帧，其 `push {r2,r3,r4,lr}` 把调用者 LR 精确放在
`[sp+12]` —— 由此精确反查到上述工厂函数。

**已排除的假设**：
- 任何 emulator server/exec 向 AknIconServer 返回 -5：三处拦截
  （`ipc_context::complete`、`session_send_general` 结果、`execute_commands`
  命令）全部无命中——-5 是 guest 客户端本地产生。
- 屏幕 display mode 不对：强制 `scr.disp_mode = color16ma` 后，AknIconServer
  创建的图标 bitmap 仍是 `color64k(7)` 色图 + `gray256(4)` 掩码（标准 S60 图标
  格式，与真机一致、与屏幕模式无关），菜单仍不渲染——所以问题在 NVG **内部
  渲染面**而非图标 bitmap 或屏幕模式。

**工厂入参的精确值（已用 dyncom block-dispatch 处插桩取到）**：工厂入参
`r0 = 7 = color64k (16bpp)`。即 guest 把图标 bitmap 的 display mode（color64k）
传给 32bpp-only 工厂 → 被拒。

**display mode 为何是 color64k**：AknIconServer 读 `z:\resource\akniconsrv.rsc`
的 "preferred icon depth"（值 0 → 64K color），按标准 S60 图标格式创建
`color64k` 色图 + `gray256` 掩码（与真机一致）。真机 5800 的 NVG 走硬件/另一条
路径不碰这个 32bpp-only 软件工厂；EKA2L1 无该路径，guest 落到软件工厂 → 拒绝。
（注：改 `scr.disp_mode` 无效，因 wsini `WINDOWMODE Color16MA` 本就是 0xC，
图标 64k 来自 rsc 而非屏幕模式。）

## 修复路径与现状

EKA2L1 **本就有一个 icon HLE**（`akn_icon_server`，`services/src/ui/icon/`），
按服务名拦截 `!AknIconServer`、可绕过坏掉的 guest NVG 软件路径。但它在
`services/src/init.cpp:246` 被 **注释禁用**（`//CREATE_SERVER(sys, akn_icon_server)`），
原因实测确认：

1. **启用即崩溃**：`akn_icon_server::retrieve_icon → fbs_server::create_bitmap
   → bitwise_bitmap::construct → do_white_fill` 触发 `SIGBUS`
   (KERN_PROTECTION_FAILURE，写入无效 bitmap buffer)，boot 阶段即 crash。
2. **功能不完整**：`retrieve_icon` 只 `create_bitmap` 出空 bitmap，**不解码/
   渲染真实图标内容**（无 NVG/MBM rasterize）。
3. **全局启用会回归**：S60v3（N95，MBM 位图图标）的 guest AknIconServer 本可
   正常渲染真图标，被 HLE 拦截后会变空白。

**因此真正修复是一个独立的中大型功能**，含三件事且有取舍：
(a) 修 icon HLE 的 `create_bitmap`/`do_white_fill` SIGBUS；
(b) 在 HLE 内实现真实图标渲染（NVG/SVG/MBM → bitmap，可复用现有
`loader::convert_n_to_svg` 等）；
(c) 合理 gate（仅在 guest 软件 NVG 会失败的场景拦截，避免回归 MBM 图标）。
或者，另一条路是让 guest 软件 NVG 路径拿到 32bpp 渲染面（更难，guest 内部）。

**离线分析素材**（runtime base）：图形工厂模块 @~0x8084Cxxx（工厂入口
`0x8084E7A8`、leave 点 `0x8084E80C: mvn r0,#4; bl User::Leave`）、`bitgdi`类
模块 @~0x80697xxx（DisplayMode 查询 @0x80697A0B）、`euser.dll`@0x8039D668
（`User::Leave`@+0x126EE、`SendReceive`@+0xFA8A）、`ws32.dll`@0x806C9268、
`102827cf.dll`(NVG 解码 ECom 插件)@0x82EA59B8。dyncom 取工厂入参的插桩点：
`arm_dyncom_interpreter.cpp` block-dispatch 处 `if (cpu->Reg[15]==<entry>) log Reg[0]`。

## 诊断方法备忘

- iOS sim 驱动 guest UI：用 `xcodebuildmcp ui-automation snapshot-ui` 取
  elementRef，再 `ui-automation tap`（坐标 tap / `simulator tap` 不存在）。
- 开 window/kernel trace：`config.yml` 的 `log-filter` 设
  `"*:trace CPU:off CPU.DynCom:off CPU.12L1R:off"`（直接 `*:trace` 会被 iOS
  bridge 覆盖；CPU off 用来压住 VFP trace 洪水）。验证完恢复 `*:warn`。
- 定位 guest leave 来源：在 `svc.cpp` 的 `leave_start` 临时打印线程名 + 把
  PC/LR 用 `get_codeseg_list()` 反查所属 codeseg（线程名 `!AknIconServer` 即
  来自此法）。诊断代码不入最终提交。
