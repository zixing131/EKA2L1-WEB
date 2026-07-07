/*
 * Copyright (c) 2019 EKA2L1 Team.
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

#include <common/types.h>

#include <mutex>
#include <string>
#include <vector>

namespace eka2l1::config {
    struct state;
}

namespace eka2l1 {
    struct device {
    private:
        std::uint32_t cached_flags_;
        bool flag_inited_ = false;

        enum {
            DEVICE_FLAG_S80 = 1 << 0
        };

        void init_flags();

    public:
        epocver ver;
        std::string firmware_code;
        std::string manufacturer;
        std::string model;
        std::vector<int> languages;
        std::uint32_t machine_uid;
        int default_language_code;

        explicit device(epocver ver, std::string firmware_code, std::string manufacturer, std::string model)
            : ver(ver)
            , firmware_code(firmware_code)
            , manufacturer(manufacturer)
            , model(model)
            , machine_uid(0)
            , default_language_code(-1) {
        }

        bool is_s80();
    };

    enum add_device_error {
        add_device_none = 0,
        add_device_existed
    };

    /*! \brief A manager for all installed devices on this emulator
    */
    class device_manager {
        std::vector<device> devices;
        config::state *conf;

        std::int32_t current_index;

    public:
        std::mutex lock;

        explicit device_manager(config::state *conf);
        ~device_manager();

        std::vector<device> &get_devices() {
            return devices;
        }

        std::size_t total() {
            return devices.size();
        }

        device *get_current() {
            if ((current_index < 0) || (current_index >= devices.size())) {
                return nullptr;
            }

            return &devices[current_index];
        }

        std::int32_t get_current_index() const {
            return current_index;
        }

        device *lastest() {
            if (devices.empty())
                return nullptr;

            return &devices.back();
        }

        void save_devices();
        void load_devices();
        void clear();

        bool set_current(const std::string &firmcode);
        bool set_current(const std::uint8_t idx);

        add_device_error add_new_device(const std::string &firmcode, const std::string &model, const std::string &manufacturer, const epocver ver, const std::uint32_t machine_uid);

        bool delete_device(const std::string &firmcode);

        /*! \brief Detect and clean up a stale ("ghost") device registration.
         *
         * A registration can outlive its firmware payload if a previous
         * delete/reinstall was interrupted before both sides (devices.yml
         * and the drives/z/<firmcode>/ payload) were removed together (e.g.
         * a browser tab killed mid-uninstall, or a persistence layer that
         * updates the registry and the file tree in separate steps). Without
         * this check, installers rejecting on "device already exists" would
         * permanently wedge that firmware code even though nothing usable is
         * left of it.
         *
         * \param firmcode        The firmware code found duplicated.
         * \param drives_z_root   The drives/z resident root path (the
         *                        directory that contains firmcode-named
         *                        subfolders for each installed device).
         *
         * \returns true if \p firmcode is a genuine, still-installed
         *          duplicate (caller should still reject); false if there
         *          was no registration, or a ghost one was found and removed
         *          (caller may proceed with the install).
        */
        bool heal_ghost_registration(const std::string &firmcode, const std::string &drives_z_root);

        /*! \brief Get the device with the given firmware code.
         *
         * You should avoid method that involves comparing firmware code, since
         * the comparsion is case-sensitive. Use listing and index instead.
         * 
         * Not thread-safe.
         * 
         * \returns nullptr if the device can't be found
        */
        device *get(const std::string &firmcode);

        /*! \brief Get the device with the given index.
         *
         * Not thread-safe.
         * 
         * \returns nullptr if index out of range.
        */
        device *get(const std::uint8_t index);
    };
}
