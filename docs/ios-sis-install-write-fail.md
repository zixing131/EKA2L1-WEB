# SIS 安装在 iOS 上静默写失败

> 来源：阶段 3.2.3（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决。
>
> **一句话结论**：`sis_script_interpreter` 在 case-sensitive 平台上把已解析出的 **host 绝对路径**整条 `lowercase_string`，iOS 容器路径含 `/Users/…` 和 UUID，小写后变成不存在的 `/users/…` 导致 payload 写失败（registry 却成功，形成"安装成功但 E 盘为空"的假阳性）；修法是只 lower-case Symbian 虚拟路径（`e:\...`）再交给 `io->get_raw_path` 解析、绝不 lower-case host 路径，并让 `get_raw_path` 失败时传播 false。

- 现象：iOS 模拟器上点 The Final Battle.sis → 日志按预期打印 `[Loader]: File detected:` 列出所有 SIS 内文件 → `[Packages]: Package Final Battle registering with UID: 0xa0003c62` → `[Packages]: Installation done!` → `[Service.Applist]: Loading app registries` / `Done loading!`（增量 = 0）。但实际检查 sandbox：
  - `Documents/data/drives/c/sys/install/sisregistry/a0003c62/{00000000.reg,00000000_0000.ctl}` **已写入** —— 说明 sisregistry 路径正常。
  - `Documents/data/drives/e/` **完全空** —— SIS payload（`FBattle.RSC` / `FBattle_reg.RSC` / `FBattle.exe` / 一堆 `.wav` `.mod`）一个文件都没落地。
  - applist 重扫看的是 `e:\Private\10003a3f\import\apps\*.rsc` 等路径（`src/emu/services/src/applist/applist.cpp:438`），既然 e 是空的，自然 0 增量。
- 范围判断：sisregistry 是 EKA2L1 内部维护的元数据（直接 host C++ write），payload 走的是 `io_system::open(write)` 进 vfs 写入；两条路径都最终落到 `data/drives/<X>/...`。前者成功后者失败 → 说明 vfs 的 write 路径在 iOS 上有问题，可能：
  1. `io->open(...., WRITE_MODE)` 没有自动 `mkdir -p` 父目录，iOS 上 `data/drives/e/Resource/Apps/` 不存在直接 open 失败；
  2. sis_script_interpreter 把 `!:` 替换成 `e:` 后，路径生成大小写跟 sandbox 实际期望对不上（3.2 阶段已有 vfs case-insensitive read fallback，但 write 端没改）；
  3. install_drive=drive_e 在 iOS 上没真挂载成可写（看 `src/emu/ios/Bridge/IosEmulator.mm::startWithDocumentsPath:` 的 `sys->mount(drive_e, ..., io_attrib_removeable)` —— removeable + iOS sandbox 组合下 vfs 可能拒绝写）。
- 根因：`sis_script_interpreter` 在 case-sensitive 平台上先用 VFS 把 `e:\...` 解析成 host 绝对路径，然后把整条 **host path** `lowercase_string`。iOS simulator app container 路径包含 `/Users/.../Library/...` 和 UUID，lowercase 后变成不存在的 `/users/...`，`wo_std_file_stream` 打不开目标；安装器没有把这个 write 失败传播出去，于是 registry 成功、payload 为空。
- 修法：只 lower-case Symbian 虚拟路径（`e:\resource\apps\...`）再交给 `io->get_raw_path` 解析，绝不 lower-case host 绝对路径；同时 `get_raw_path` 失败时返回 interpret false，避免以后继续出现"安装成功但文件为空"的假阳性。
- 验收：iOS sim 上跑 The Final Battle.sis 安装 → `drives/e/resource/apps/fbattle.rsc` / `drives/e/sys/bin/fbattle.exe` / `drives/e/private/10003a3f/import/apps/fbattle_reg.rsc` 均存在 → applist 重扫出 `Final Battle, uid=0xA0003C62`。
