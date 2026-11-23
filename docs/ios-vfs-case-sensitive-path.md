# iOS sandbox 内 vfs 路径解析 case-sensitive，混合大小写 ROM 文件名失败

> 来源：阶段 3 修复清单 #2（[`IOS_PORTING_TASKS.md`](IOS_PORTING_TASKS.md)）。状态：✅ 已解决。
>
> **一句话结论**：iOS 下 `is_system_case_insensitive()` 返回 false → 整条路径被强制小写，而 host（app 进程视角）case-sensitive，混合大小写的 ROM 文件（如 `Wsini.ini`）找不到；修法是在 `get_real_physical_path` 的 iOS 分支里逐级用目录迭代 + `compare_ignore_case` 解析回真实大小写。

3.1 修复 SIGBUS 后第一次跑 mount，启动日志看到 `[Service.Window]: Loading wsini file broke with code -1`，path 是 `data/drives/z/rm-320/system/data/wsini.ini`（全小写），但 host 真实文件名是 `Wsini.ini`。stage 2 #13 已经统一了顶层目录大小写，但 ROM tree 里很多文件（如 `Wsini.ini` / `SkinExclusions.ini` / `Iva_base.dof`）保留了原始混合大小写。`physical_file_system::get_real_physical_path` 走 `is_system_case_insensitive()` 的桌面默认值在 iOS 下返回 false → 整条路径被 `lowercase_ucs2_string` 强制小写 → host 那侧（iOS sim app 进程视角 = case-sensitive）找不到。

修法：在 `src/emu/vfs/src/vfs.cpp::get_real_physical_path` 末尾、iOS 分支下，若 lowercased path 不存在，则从 mount root 开始逐级用 `make_directory_iterator` + `compare_ignore_case` 把每一段解析回真实大小写，再返回新的 final_path。只在 iOS 触发，桌面/Android/Win32 行为完全不变。

验证：再跑 mount，`Loading wsini file broke` 日志消失。后续残留的 `Fail to load languages.txt`、`Failed to open thread panic blacklist file` 走的是裸 `dynamic_ifile`/`ro_std_file_stream` 直读 host 路径（不经 vfs），二者本身都是路径合法但 host 文件不存在的"信息缺失"类，不属于本条修复范围。
