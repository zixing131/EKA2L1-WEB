/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project
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

namespace eka2l1::hos {
    class launcher;
}

namespace eka2l1::drivers::ui {
    // The active HarmonyOS launcher, set during emulator::stage_one(). The core
    // calls the drivers::ui dialog functions (declared in drivers/ui/input_dialog.h)
    // which forward to this instance.
    extern eka2l1::hos::launcher *launcher_instance;
}
