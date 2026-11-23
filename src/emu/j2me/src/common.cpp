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

#include <common/path_types.h>
#include <j2me/common.h>
#include <j2me/applist.h>

#include <common/algorithm.h>
#include <common/archive.h>
#include <common/buffer.h>
#include <common/fileutils.h>
#include <common/path.h>
#include <common/pystr.h>
#include <common/log.h>
#include <config/config.h>

#include <fmt/format.h>
#include <memory>
#include <sstream>

namespace eka2l1::j2me {
    void set_jad_keypad(std::string &jad, std::string_view mode) {
        if (mode != "no" && mode != "gameactions" && mode != "navigationkeys") return;
        constexpr std::string_view key = "Nokia-MIDlet-On-Screen-Keypad:";
        std::string output;
        std::istringstream input(jad);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, key.size(), key) != 0 && !line.empty()) {
                output += line + "\r\n";
            }
        }
        output.append(key);
        output += " ";
        output.append(mode);
        output += "\r\n";
        jad = std::move(output);
    }

    std::string jad_from_manifest(std::string_view manifest, std::string_view jar_url, std::uint64_t jar_size) {
        // Manifest continuation lines are not JAD attributes. Unfold the main
        // section before handing it to the ROM installer, including splits
        // inside permission names or UTF-8 sequences. Per-entry sections must
        // not become suite properties.
        std::string jad;
        std::string attribute;
        const auto append_attribute = [&]() {
            if (!attribute.empty()
                && attribute.compare(0, 15, "MIDlet-Jar-URL:") != 0
                && attribute.compare(0, 16, "MIDlet-Jar-Size:") != 0) {
                jad += attribute;
                jad += "\r\n";
            }
            attribute.clear();
        };
        while (!manifest.empty()) {
            const std::size_t end = manifest.find_first_of("\r\n");
            const std::string_view line = manifest.substr(0, end);
            if (end == std::string_view::npos) {
                manifest = {};
            } else {
                const std::size_t newline_size = (manifest[end] == '\r'
                    && end + 1 < manifest.size() && manifest[end + 1] == '\n') ? 2 : 1;
                manifest.remove_prefix(end + newline_size);
            }
            if (line.empty()) {
                break;
            }
            if (line.front() == ' ' && !attribute.empty()) {
                attribute.append(line.data() + 1, line.size() - 1);
            } else {
                append_attribute();
                attribute.assign(line.data(), line.size());
            }
        }
        append_attribute();
        jad += "MIDlet-Jar-URL: ";
        jad.append(jar_url.data(), jar_url.size());
        jad += fmt::format("\r\nMIDlet-Jar-Size: {}\r\n", jar_size);
        return jad;
    }

    install_error create_jad_fake_link_from_jar(const host_path &jar_path, std::string &jad_content) {
        static const char *MANIFEST_FILE_PATH = "META-INF/MANIFEST.MF";

        std::vector<char> manifest;

        if (!common::read_archive_entry(jar_path, MANIFEST_FILE_PATH, manifest, common::MB(1))) {
            return INSTALL_ERROR_JAR_INVALID;
        }

        jad_content = jad_from_manifest(std::string_view(manifest.data(), manifest.size()),
            "https://12z1.com/jar/fake.jar", common::file_size(jar_path));

        return INSTALL_ERROR_JAR_SUCCESS;
    }

    install_error extract_icon_to_store(const host_path &jar_path, const config::state &conf, const app_entry &entry, const std::string &icon_entry, host_path &final_real_path) {
        const std::string fname = eka2l1::filename(icon_entry);
        const host_path relative_path(fmt::format("j2me/{}_{}_{}", common::lowercase_string(entry.name_),
            common::lowercase_string(entry.version_), fname));
        const host_path storing_file(eka2l1::add_path(conf.storage, relative_path));

        std::vector<char> icon;

        if (!common::read_archive_entry(jar_path, icon_entry, icon)) {
            LOG_WARN(J2ME, "Can't extract icon file with path {} for storing. Ignoring...", icon_entry);

            final_real_path.clear();
            return INSTALL_ERROR_JAR_SUCCESS;
        }

        common::create_directories(eka2l1::host_path{eka2l1::file_directory(storing_file)});
        common::wo_std_file_stream out_file(storing_file, true);

        if (!out_file.valid() || (out_file.write(icon.data(), icon.size()) != icon.size())) {
            LOG_WARN(J2ME, "Can't open storing icon file!");

            final_real_path.clear();
            return INSTALL_ERROR_JAR_SUCCESS;
        }

        final_real_path = relative_path;
        return INSTALL_ERROR_JAR_SUCCESS;
    }
    
    install_error get_app_entry(const host_path &jar_path, app_entry &entry, std::string &jad_content, int &midp_ver, std::string &icon_entry) {
        install_error return_err = create_jad_fake_link_from_jar(jar_path, jad_content);

        if (return_err != INSTALL_ERROR_JAR_SUCCESS) {
            return return_err;
        }

        std::istringstream jad_stream(jad_content);
        std::string line;

        midp_ver = 2;

        // Use this getline, it will detects platforms newline
        while (std::getline(jad_stream, line)) {
            std::size_t key_sep = line.find_first_of(':');
            if (key_sep == std::string::npos) {
                continue;
            }

            std::string key = common::trim_spaces(line.substr(0, key_sep));
            std::string val = common::trim_spaces(line.substr(key_sep + 1));
            if (key == "MIDlet-Name") {
                entry.name_ = val;
            } else if (key == "MIDlet-Version") {
                entry.version_ = val;
            } else if (key == "MIDlet-Vendor") {
                entry.author_ = val;
            } else if (key == "MIDlet-1") {
                auto comps = common::pystr(val).split(",");
                if (comps.size() >= 1) {
                    entry.title_ = comps[0].strip().std_str();
                }
                if (comps.size() >= 2) {
                    icon_entry = comps[1].strip().std_str();
                }
            } else if (key == "MicroEdition-Profile") {
                if (val == "MIDP-1.0") {
                    midp_ver = 1;
                }
            }
        }

        if (!icon_entry.empty() && eka2l1::is_separator(icon_entry[0])) {
            icon_entry.erase(icon_entry.begin());
        }

        entry.original_title_ = entry.title_;

        return INSTALL_ERROR_JAR_SUCCESS;
    }
}
