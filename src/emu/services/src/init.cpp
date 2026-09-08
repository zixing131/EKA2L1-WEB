/*
 * Copyright (c) 2018 EKA2L1 Team
 * 
 * This file is part of EKA2L1 project
 * (see bentokun.github.com/EKA2L1).
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

#include <common/algorithm.h>
#include <common/path.h>
#include <common/platform.h>

#include <memory>
#include <unordered_map>

#include <services/accessory/accessory.h>
#include <services/alarm/alarm.h>
#include <services/applist/applist.h>
#include <services/audio/alf/alf.h>
#include <services/audio/keysound/keysound.h>
#include <services/audio/mmf/audio.h>
#include <services/audio/mmf/dev.h>
#include <services/backup/backup.h>
#include <services/bluetooth/bt.h>
#include <services/bluetooth/btman.h>
#include <services/camera/camera.h>
#include <services/centralrepo/centralrepo.h>
#include <services/comm/comm.h>
#include <services/domain/domain.h>
#include <services/drm/helper.h>
#include <services/drm/notifier/notifier.h>
#include <services/drm/rights/rights.h>
#include <services/etel/etel.h>
#include <services/fbs/fbs.h>
#include <services/featmgr/featmgr.h>
#include <services/fs/fs.h>
#include <services/goommonitor/goommonitor.h>
#include <services/hwrm/hwrm.h>
#include <services/internet/connmonitor.h>
#include <services/internet/nifman.h>
#include <services/loader/loader.h>
#include <services/msv/msv.h>
#include <services/notifier/notifier.h>
#include <services/posix/posix.h>
#include <services/redir/redir.h>
#include <services/remcon/remcon.h>
#include <services/sensor/sensor.h>
#include <services/shutdown/shutdown.h>
#include <services/sisregistry/sisregistry.h>
#include <services/sms/settings.h>
#include <services/sms/sa/sa.h>
#include <services/sms/sendas/sendas.h>
#include <services/socket/server.h>
#include <services/sysagt/sysagt.h>
#include <services/timezone/timezone.h>
#include <services/ui/cap/oom_app.h>
#include <services/ui/eikappui.h>
#include <services/ui/icon/icon.h>
#include <services/ui/skin/server.h>
#include <services/ui/view/view.h>
#include <services/uiss/uiss.h>
#include <services/unipertar/unipertar.h>
#include <services/window/window.h>
#include <services/host_launch.h>

#include <services/init.h>
#include <system/epoc.h>
#include <utils/locale.h>
#include <utils/system.h>

#include <config/config.h>
#include <system/devices.h>

#include <vector>

#if EKA2L1_PLATFORM(WIN32)
#include <Windows.h>
#endif

#define CREATE_SERVER_D(sys, svr, ...)                                                 \
    std::unique_ptr<service::server> temp = std::make_unique<svr>(sys, ##__VA_ARGS__); \
    sys->get_kernel_system()->add_custom_server(temp)

#define CREATE_SERVER(sys, svr, ...)                  \
    temp = std::make_unique<svr>(sys, ##__VA_ARGS__); \
    sys->get_kernel_system()->add_custom_server(temp)

#define DEFINE_INT_PROP_D(sys, category, key, data)                            \
    property_ptr prop = sys->get_kernel_system()->create<service::property>(); \
    prop->first = category;                                                    \
    prop->second = key;                                                        \
    prop->define(service::property_type::int_data, 0);                         \
    prop->set_int(data);

#define DEFINE_INT_PROP(sys, category, key, data)                 \
    prop = sys->get_kernel_system()->create<service::property>(); \
    prop->first = category;                                       \
    prop->second = key;                                           \
    prop->define(service::property_type::int_data, 0);            \
    prop->set_int(data);

#define DEFINE_BIN_PROP_D(sys, category, key, size, data)                      \
    property_ptr prop = sys->get_kernel_system()->create<service::property>(); \
    prop->first = category;                                                    \
    prop->second = key;                                                        \
    prop->define(service::property_type::bin_data, size);                      \
    prop->set(data);

#define DEFINE_BIN_PROP(sys, category, key, size, data)           \
    prop = sys->get_kernel_system()->create<service::property>(); \
    prop->first = category;                                       \
    prop->second = key;                                           \
    prop->define(service::property_type::bin_data, size);         \
    prop->set(data);

namespace eka2l1::epoc {
    epoc::locale get_locale_info() {
        epoc::locale locale;

        // TODO: Move to common
#if EKA2L1_PLATFORM(WIN32)
        locale.country_code_ = static_cast<int>(GetProfileInt("intl", "iCountry", 0));
#endif

        // TODO: These are stubbed!
        // See in relation: CLocale::MonetaryLoadLocaleL in ossrv, openenvcore's libc in file localeinfo.cpp
        locale.clock_format_ = epoc::clock_digital;
        locale.start_of_week_ = epoc::monday;
        locale.date_format_ = epoc::date_format_america;
        locale.time_format_ = epoc::time_format_twenty_four_hours;
        locale.universal_time_offset_ = -14400;
        locale.device_time_state_ = epoc::device_user_time;
        locale.decimal_separator_ = '.';
        locale.thousands_separator_ = ',';
        locale.negative_currency_format_ = epoc::negative_currency_leading_minus_sign;

        locale.time_separator_[0] = 0;
        locale.time_separator_[1] = ':';
        locale.time_separator_[2] = ':';
        locale.time_separator_[3] = 0;

        locale.date_separator_[0] = 0;
        locale.date_separator_[1] = '/';
        locale.date_separator_[2] = '/';
        locale.date_separator_[3] = 0;

        return locale;
    }

    // TDayName / TMonthName / TDateSuffix read LOCALE_LANG_KEY as arrays of
    // pointers to NUL-terminated UTF-16 strings. Publishing the property with
    // null tables makes Get succeed and skip euser's ROM fallback, then
    // ldr [0, day*4] becomes KERN-EXEC 3 (J9 formats timestamps this way).
    static address put_locale_u16_string(kernel_system *kern, const char16_t *text) {
        std::size_t units = 0;
        while (text[units] != 0) {
            units++;
        }
        units++;
        return kern->put_global_kernel_binary(
            reinterpret_cast<const std::uint8_t *>(text), units * sizeof(char16_t));
    }

    static address put_locale_u16_table(kernel_system *kern, const char16_t *const *texts,
        const std::size_t count) {
        std::vector<address> addrs(count);
        for (std::size_t i = 0; i < count; i++) {
            addrs[i] = put_locale_u16_string(kern, texts[i]);
        }
        return kern->put_static_array(addrs.data(), addrs.size());
    }

    static void fill_locale_language_tables(kernel_system *kern, epoc::locale_language &lang) {
        static const char16_t *DATE_SUFFIX[] = {
            u"st", u"nd", u"rd", u"th", u"th", u"th", u"th", u"th", u"th", u"th",
            u"th", u"th", u"th", u"th", u"th", u"th", u"th", u"th", u"th", u"th",
            u"st", u"nd", u"rd", u"th", u"th", u"th", u"th", u"th", u"th", u"th",
            u"st"
        };
        static const char16_t *DAYS[] = {
            u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday", u"Sunday"
        };
        static const char16_t *DAYS_ABB[] = {
            u"Mon", u"Tue", u"Wed", u"Thu", u"Fri", u"Sat", u"Sun"
        };
        static const char16_t *MONTHS[] = {
            u"January", u"February", u"March", u"April", u"May", u"June",
            u"July", u"August", u"September", u"October", u"November", u"December"
        };
        static const char16_t *MONTHS_ABB[] = {
            u"Jan", u"Feb", u"Mar", u"Apr", u"May", u"Jun",
            u"Jul", u"Aug", u"Sep", u"Oct", u"Nov", u"Dec"
        };
        static const char16_t *AM_PM[] = { u"am", u"pm" };
        static const char16_t *MSGS[] = {
            u"Retry", u"Stop", u"Put the disk back", u"or data will be lost",
            u"Batteries too low", u"Cannot complete write to disk",
            u"Disk error - cannot complete write", u"Retry or data will be lost",
            u"Chimes", u"Rings", u"Signal", u"Internal",
            u"External(01)", u"External(02)", u"External(03)", u"External(04)",
            u"External(05)", u"External(06)", u"External(07)", u"External(08)",
            u"Socket(01)", u"Socket(02)", u"Socket(03)", u"Socket(04)"
        };

        lang.date_suffix_table = eka2l1::ptr<char>(put_locale_u16_table(kern, DATE_SUFFIX, 31));
        lang.day_table = eka2l1::ptr<char>(put_locale_u16_table(kern, DAYS, 7));
        lang.day_abb_table = eka2l1::ptr<char>(put_locale_u16_table(kern, DAYS_ABB, 7));
        lang.month_table = eka2l1::ptr<char>(put_locale_u16_table(kern, MONTHS, 12));
        lang.month_abb_table = eka2l1::ptr<char>(put_locale_u16_table(kern, MONTHS_ABB, 12));
        lang.am_pm_table = eka2l1::ptr<char>(put_locale_u16_table(kern, AM_PM, 2));
        lang.msg_table = eka2l1::ptr<std::uint16_t>(put_locale_u16_table(kern, MSGS, 24));
    }

    static void initialize_system_properties(eka2l1::system *sys, eka2l1::config::state *cfg) {
        auto lang = epoc::locale_language{ epoc::lang_english, 0, 0, 0, 0, 0, 0, 0 };
        auto locale = epoc::get_locale_info();
        auto &dvcs = sys->get_device_manager()->get_devices();
        kernel_system *kern = sys->get_kernel_system();

        if (dvcs.size() > cfg->device) {
            auto &dvc = dvcs[cfg->device];

            if (cfg->language == -1) {
                lang.language = static_cast<epoc::language>(dvc.default_language_code);
            } else {
                lang.language = static_cast<epoc::language>(cfg->language);
            }
        }

        fill_locale_language_tables(kern, lang);

        epoc::locale_locale_settings locale_settings;
        locale_settings.locale_extra_settings_dll_ptr = 0;
        locale_settings.currency_symbols[0] = '$';
        locale_settings.currency_symbols[1] = '\0';

        // Unknown key, testing show that this prop return 65535 most of times
        // The prop belongs to HAL server, but the key usuage is unknown. (TODO)
        DEFINE_INT_PROP_D(sys, epoc::SYS_CATEGORY, epoc::UNK_KEY1, 65535);
        DEFINE_INT_PROP(sys, epoc::SYS_CATEGORY, epoc::PHONE_POWER_KEY, system_agent_state_on);
        DEFINE_INT_PROP(sys, epoc::SYS_CATEGORY, epoc::SOFTWARE_INSTALL_KEY, 0);
        DEFINE_INT_PROP(sys, epoc::SYS_CATEGORY, epoc::SOFTWARE_LASTEST_UID_INSTALLATION, 0);

        // Published during the native S60 boot sequence. These enum-based
        // properties use 100 as their defined "uninitialised" value; zero is
        // outside every public startup-state enum and makes the ROM starter
        // reject its first transition.
        for (std::uint32_t key = 0x41; key <= 0x4F; ++key) {
            property_ptr boot_state = sys->get_kernel_system()->create<service::property>();
            boot_state->first = 0x101F8766;
            boot_state->second = key;
            boot_state->define(service::property_type::int_data, 0);
            boot_state->set_int(100);
        }

        // Starter asks SplashScreen to close through this separate startup
        // property after the active-idle phase is ready.  Defining it here
        // lets the ROM complete the screen's outstanding subscription instead
        // of having the frontend terminate the process.
        property_ptr splash_shutdown = sys->get_kernel_system()->create<service::property>();
        splash_shutdown->first = 0x101F8766;
        splash_shutdown->second = 0x301;
        splash_shutdown->define(service::property_type::int_data, 0);
        splash_shutdown->set_int(100);

        // Avkon's shell and Menu read these state slots while registering their
        // window groups. They are published by the device's UI bootstrap on a
        // phone, before either executable runs.
        DEFINE_INT_PROP(sys, 0x101F8773, 0x10, 0);
        DEFINE_INT_PROP(sys, 0x10207218, 0x7, 0);

        // SysAp is normally preceded by several device-resident daemons. Its
        // early boot reads their published integer state; provide their reset
        // values until those daemons themselves are emulated.
        static const std::pair<std::uint32_t, std::uint32_t> sysap_boot_props[] = {
            { 0x101F75B6, 0x100052CD }, { 0x101F75B6, 0x100052CE },
            { 0x101F75B6, 0x100052D1 }, { 0x101F75B6, 0x100052DB },
            { 0x101F75B6, 0x100052FF }, { 0x101F75B6, 0x100052FA },
            { 0x101F8766, 0x32 }, { 0x101F8766, 0x401 },
            { 0x101F8767, 0x102 }, { 0x101F8767, 0x103 }, { 0x101F8767, 0x104 },
            { 0x101F8767, 0x106 }, { 0x101F8767, 0x107 }, { 0x101F8767, 0x108 },
            { 0x101F8767, 0x109 }, { 0x101F8767, 0x110 }, { 0x101F8767, 0x112 },
            { 0x101F8767, 0x113 }, { 0x101F8767, 0x114 }, { 0x101F8767, 0x115 },
            { 0x101F8767, 0x116 }, { 0x101F8767, 0x117 }, { 0x101F8767, 0x201 },
            { 0x101F8767, 0x202 }, { 0x101F8767, 0x203 }, { 0x101F8767, 0x204 },
            { 0x101F8767, 0x501 }, { 0x101F8EC5, 0x2 }, { 0x102029AC, 0x1 },
            { 0x10202999, 0x1 }, { 0x10202999, 0x2 }, { 0x10205047, 0x102 },
            { 0x101F9A0B, 0x1 }, { 0x101F7989, 0x10282FAF },
            { 0x101F8771, 0x3 }, { 0x101F97B2, 0x2 }
        };
        for (const auto &[category, key] : sysap_boot_props) {
            property_ptr state = sys->get_kernel_system()->create<service::property>();
            state->first = category;
            state->second = key;
            state->define(service::property_type::int_data, 0);
            state->set_int(0);
        }

        // Published by the secure backup engine on a real device. Clients that watch the
        // backup state (File manager's backup engine for one) read it while constructing and
        // leave with KErrNotFound if it was never defined, taking the whole app down.
        DEFINE_INT_PROP(sys, epoc::SYS_CATEGORY, epoc::BACKUP_RESTORE_KEY, epoc::BACKUP_RESTORE_NORMAL_STATE);

        // From Domain Server request
        DEFINE_INT_PROP(sys, 0x1020e406, 0x250, 0);

        // Without these SysUtil's critical-disk-space check finds no threshold at all
        // and leaves, so a caller that checks free space before writing -- Camera
        // saving a photo, for one -- never gets to the write.
        DEFINE_INT_PROP(sys, epoc::DISK_LEVEL_CATEGORY, epoc::RAM_DISK_CRITICAL_THRESHOLD_KEY,
            epoc::RAM_DISK_CRITICAL_THRESHOLD);
        DEFINE_INT_PROP(sys, epoc::DISK_LEVEL_CATEGORY, epoc::OTHER_DISK_CRITICAL_THRESHOLD_KEY,
            epoc::OTHER_DISK_CRITICAL_THRESHOLD);

        DEFINE_BIN_PROP(sys, epoc::SYS_CATEGORY, epoc::LOCALE_LANG_KEY, sizeof(epoc::locale_language), lang);
        DEFINE_BIN_PROP(sys, epoc::SYS_CATEGORY, epoc::LOCALE_DATA_KEY, sizeof(epoc::locale), locale);
        DEFINE_BIN_PROP(sys, epoc::SYS_CATEGORY, epoc::LOCALE_LOCALE_SETTINGS_KEY, sizeof(epoc::locale_locale_settings), locale_settings);
    }
}

namespace eka2l1 {
    namespace service {
        // Mostly replace startup process of a normal EPOC startup
        void init_services(system *sys) {
            if (sys->get_kernel_system()->is_eka1()) {
                kernel_system *kern = sys->get_kernel_system();
                auto posix_servers = std::make_shared<std::unordered_map<kernel::uid, posix_server *>>();

                kern->register_codeseg_loaded_callback([sys, posix_servers](const std::string &, kernel::process *process,
                                                           codeseg_ptr code_segment) {
                    // Patch libraries are attached globally while the device boots and do not
                    // belong to a guest process.
                    if (!process) {
                        return;
                    }

                    const auto existing_server = posix_servers->find(process->unique_id());
                    if (existing_server != posix_servers->end()) {
                        // EKA1 GUI applications run inside AppRun.exe. Once its .app image is
                        // attached, that image determines the POSIX current drive rather than
                        // AppRun's Z: drive.
                        if (common::lowercase_ucs2_string(eka2l1::path_extension(code_segment->get_full_path()))
                            == u".app") {
                            existing_server->second->update_executable_path(code_segment->get_full_path());
                        }
                        return;
                    }

                    if (code_segment != process->get_codeseg()) {
                        return;
                    }

                    std::unique_ptr<posix_server> server = std::make_unique<posix_server>(sys, process, code_segment->get_full_path());
                    (*posix_servers)[process->unique_id()] = server.get();
                    std::unique_ptr<service::server> generic_server = std::move(server);
                    sys->get_kernel_system()->add_custom_server(generic_server);
                });

                kern->register_process_exit_callback([posix_servers](kernel::process *process) {
                    const auto server = posix_servers->find(process->unique_id());
                    if (server == posix_servers->end()) {
                        return;
                    }

                    // Process exit callbacks run before its session handles are released.
                    // Removing the HLE server here would leave those sessions pointing at
                    // freed memory. The server has a process-unique name, so only remove it
                    // from the lookup map and let normal kernel teardown own its lifetime.
                    posix_servers->erase(server);
                });
            }

            CREATE_SERVER_D(sys, fs_server);
            CREATE_SERVER(sys, loader_server);
            CREATE_SERVER(sys, shutdown_server);

            if (sys->get_kernel_system()->is_eka1()) {
                CREATE_SERVER(sys, camera_server);
            }

            config::state *cfg = sys->get_config();

            CREATE_SERVER(sys, fbs_server);
            CREATE_SERVER(sys, window_server);
            CREATE_SERVER(sys, central_repo_server);
            CREATE_SERVER(sys, featmgr_server);

            if (cfg->enable_srv_rights)
                CREATE_SERVER(sys, rights_server);

            if (cfg->enable_srv_sa)
                CREATE_SERVER(sys, sa_server);

            if (cfg->enable_srv_drm)
                CREATE_SERVER(sys, drm_helper_server);

            // These needed to be HLEd
            CREATE_SERVER(sys, applist_server);
            CREATE_SERVER(sys, oom_ui_app_server);
            CREATE_SERVER(sys, hwrm_server);
            CREATE_SERVER(sys, view_server);
            CREATE_SERVER(sys, remcon_server);
            CREATE_SERVER(sys, etel_server);
            CREATE_SERVER(sys, notifier_server);
            CREATE_SERVER(sys, msv_server);

            CREATE_SERVER(sys, sensor_server);
            CREATE_SERVER(sys, connmonitor_server);
            CREATE_SERVER(sys, nifman_server);
            CREATE_SERVER(sys, drm_notifier_server);
            CREATE_SERVER(sys, sisregistry_server);
            CREATE_SERVER(sys, alarm_server);
            CREATE_SERVER(sys, socket_server);

            CREATE_SERVER(sys, comm_server);
            CREATE_SERVER(sys, bt_server);
            CREATE_SERVER(sys, btman_server);
            CREATE_SERVER(sys, accessory_server);

            // Not really sure about this one
            CREATE_SERVER(sys, keysound_server);

            CREATE_SERVER(sys, eikappui_server);
            // The AknIconServer HLE renders icons itself (lunasvg / mbm) instead of the guest
            // ROM server. It exists to work around N95-class S60v3 FP1 ROMs, whose guest icon
            // server rasterises scalable NVG menu icons through software OpenVG -- the emulator
            // has no GPU NVG, and that draw-device path only accepts 32bpp, so it leaves on the
            // standard 64K icon and aborts the Options menu. The HLE is not a complete drop-in
            // replacement, though: newer ROMs (e.g. Nokia 5320, FP2) render every icon fine via
            // the guest server but use icon-server requests the HLE doesn't fully implement, so
            // forcing them through it regresses their UI (blank Calculator). Only replace the
            // guest server where it is actually broken; let every other ROM keep its own.
            if (sys->get_symbian_version_use() == epocver::epoc93fp1) {
                CREATE_SERVER(sys, akn_icon_server);
            }
            CREATE_SERVER(sys, akn_skin_server);

            CREATE_SERVER(sys, system_agent_server);
            CREATE_SERVER(sys, unipertar_server);

            if (sys->get_symbian_version_use() >= epocver::epoc95) {
                CREATE_SERVER(sys, timezone_server);
            }

            // S60 3rd FP2's ROM System Starter already uses the Domain
            // Manager to drive its boot-resource states.  Starting this
            // service only at 9.5 leaves a 5320's SysStart waiting forever
            // before it can parse Starter_Arm.rsc.
            if (sys->get_symbian_version_use() >= epocver::epoc93fp2) {
                CREATE_SERVER(sys, dm_domain_server);
            }

            if (sys->get_symbian_version_use() <= epocver::eka2) {
                CREATE_SERVER(sys, redir_server);
                CREATE_SERVER(sys, backup_old_server);
            } else {
                CREATE_SERVER(sys, goom_monitor_server);
                CREATE_SERVER(sys, alf_streamer_server);
                // MMF server family
                {
                    std::unique_ptr<service::server> dev_serv = std::make_unique<mmf_dev_server>(sys);
                    std::unique_ptr<service::server> aud_serv = std::make_unique<mmf_audio_server>(sys,
                        reinterpret_cast<mmf_dev_server *>(dev_serv.get()));

                    kernel_system *kern = sys->get_kernel_system();
                    kern->add_custom_server(dev_serv);
                    kern->add_custom_server(aud_serv);
                }
            }

            epoc::initialize_system_properties(sys, cfg);
            init_symbian_app_launch_to_host_launch(sys);
        }
        
        void init_services_post_bootup(system *sys) {
            epoc::sms::supply_sim_settings(sys);
        }
    }
}
