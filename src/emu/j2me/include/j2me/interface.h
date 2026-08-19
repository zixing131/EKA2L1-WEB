/*
 * Copyright (c) 2022 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <j2me/common.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eka2l1 {
    class system;

    namespace kernel {
        class process;
    }
}

namespace eka2l1::j2me {
    struct app_entry;
    class app_list;

    /**
     * @brief 预置 S60v3 MIDP2 运行时需要的 C 盘目录/文件（模拟真机出厂 C: 内容）。
     *
     * SystemAMSCore/安全策略组件先读 C:\system\data\midp2\... 再回退 Z:。真机 C: 出厂
     * 即有该目录树（打开缺失文件返回 KErrNotFound，回退逻辑可识别）；干净的模拟 C: 没有
     * 目录时返回 KErrPathNotFound，导致 RunL Leave 崩溃。这里补齐目录并复制策略文件。
     *
     * @param sys 系统实例
     * @return true 表示环境就绪（或本来就就绪）
     */
    bool prepare_midp2_environment(system *sys);

    bool launch(system *sys, const std::uint32_t app_id, std::function<void(kernel::process*)> exit_cb);

    /**
     * @brief After launch() returns false: true means AMS is still coming up.
     *
     * The caller should keep the emulator running and retry launch() shortly.
     * wasm_launch_midlet maps this to -101.
     */
    bool launch_should_retry();

    /**
     * @brief Kill a hung MIDP2SilentMIDletInstall / SWInst so a new JAR install
     *        is not rejected as "already in progress".
     */
    void stop_silent_installer(system *sys);

    bool reregister_midlet(system *sys, const std::uint32_t app_id,
        std::function<void(kernel::process*)> exit_cb);

    /**
     * @brief Restage a stored MIDlet into the guest preinstall directories.
     *
     * Copies the per-MIDlet JAR/JAD to both the S60v3 MIDP2 preinstall path
     * (`E:\system\data\midp2\preinstall\`) and the later OMJ path
     * (`E:\resource\java\preinstall\`), then rewrites MIDlet-Jar-URL to a
     * local relative name. Used before AppArc inject / silent re-register.
     *
     * @param jad_path_out Filled with the staged MIDP2 JAD path on success.
     */
    bool stage_midlet_for_launch(system *sys, const std::uint32_t app_id, std::u16string &jad_path_out);
    install_error install(system *sys, const std::string &path, app_entry &entry_info,
        std::function<void(kernel::process*)> install_exit_cb = nullptr);
    bool uninstall(system *sys, const std::uint32_t app_id);

    /**
     * @brief 直接从已存储的 JAR 中提取 MIDlet 图标的原始字节（通常为 PNG）。
     *
     * 当 install 时未能把图标写入宿主存储（例如 WASM 首次安装目录缺失导致
     * extract_icon_to_store 静默失败、entry.icon_path_ 为空），前端可调用本函数
     * 实时从 per-MIDlet storage 中的 JAR 重新解析图标，无需重装。
     *
     * @param sys    系统实例
     * @param app_id MIDlet 的真实列表 ID
     * @param out    成功时填入图标文件的原始字节
     * @return true 表示成功提取到图标字节
     */
    bool extract_midlet_icon_png(system *sys, const std::uint32_t app_id, std::vector<std::uint8_t> &out);

    /**
     * @brief 检查 ROM 是否包含运行 MIDlet 所需的 Java 运行时组件。
     *
     * 支持三种启动路径：
     *   1. IBM J9（j9midps60.exe），5320 / S60v3 的实际 VM
     *   2. 独立启动器 exe（midp2midletlauncher.exe / midletlauncher.exe）
     *   3. AppArc 非原生路径（systemamscore.exe + midp2runtimev2.dll），
     *      MIDlet 注册到 AppArc 后由 StubMIDP2RecogExe.exe 以 opaque data 启动。
     *
     * @param sys 系统实例
     * @return true 表示存在可用的 MIDlet 启动路径
     */
    bool check_launch_capability(system *sys);

    // Command line fed to j9midps60.exe. Decimal -msid (TLex::Val) and
    // -app <MIDlet-1 class>. No -jcl: midp2ams Args.parse rejects it.
    std::u16string build_j9midps60_args(std::uint32_t suite_uid, const std::u16string &midlet_class);
}
