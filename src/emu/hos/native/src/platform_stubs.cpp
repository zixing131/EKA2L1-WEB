/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project - HarmonyOS port
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

#include <common/applauncher.h>
#include <common/log.h>

namespace eka2l1::common {
    bool launch_browser(const std::string &url) {
        // TODO: route to ArkTS startAbilityByType('browser', { uri }) through the
        // NAPI bridge. For now this is a no-op so guest "open URL" requests don't
        // crash the emulator.
        LOG_INFO(FRONTEND_CMDLINE, "launch_browser requested (not yet wired on OHOS): {}", url);
        return false;
    }
}
