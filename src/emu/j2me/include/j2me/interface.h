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
#include <functional>
#include <string>

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
    install_error install(system *sys, const std::string &path, app_entry &entry_info,
        std::function<void(kernel::process*)> install_exit_cb = nullptr);
    bool uninstall(system *sys, const std::uint32_t app_id);
}
