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

#include "napi/native_api.h"
#include "napi_helpers.h"
#include "xcomponent_bridge.h"

#include <hos/state.h>
#include <hos/thread.h>
#include <hos/ui_bridge.h>

#include <common/path.h>
#include <common/fileutils.h>
#include <drivers/audio/audio.h>
#include <drivers/graphics/graphics.h>
#include <drivers/sensor/sensor.h>

#include <cstring>
#include <memory>
#include <string>

using namespace eka2l1::hos::napiutil;

namespace {
    // The single emulator instance, created on the first startNative() call.
    std::unique_ptr<eka2l1::hos::emulator> g_state;

    // The native window owned by the XComponent surface (OHNativeWindow*).
    void *g_surface = nullptr;
    bool g_threads_inited = false;

    eka2l1::hos::launcher *launcher() {
        return g_state ? g_state->launcher.get() : nullptr;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static napi_value SetDirectory(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    const std::string path = get_string(env, args[0]);
    // The HAP sandbox path is a directory; cd into it so the emulator's relative
    // paths (config, drives, cache, patch) resolve under the app sandbox.
    eka2l1::common::set_current_directory(path);
    return make_undefined(env);
}

static napi_value StartNative(napi_env env, napi_callback_info info) {
    if (!g_state) {
        g_state = std::make_unique<eka2l1::hos::emulator>();
    }
    const bool ok = eka2l1::hos::emulator_entry(*g_state);
    return make_bool(env, ok);
}

static napi_value IsInitialized(napi_env env, napi_callback_info info) {
    return make_bool(env, static_cast<bool>(g_state));
}

// ---------------------------------------------------------------------------
// App list / icons
// ---------------------------------------------------------------------------

static napi_value GetApps(napi_env env, napi_callback_info info) {
    if (!launcher()) {
        return make_string_array(env, {});
    }
    return make_string_array(env, launcher()->get_apps());
}

// Returns { width, height, data: ArrayBuffer(rgba8888) } or undefined.
static napi_value icon_to_js(napi_env env, const eka2l1::hos::icon_bitmap &icon) {
    if (!icon.valid()) {
        return make_undefined(env);
    }

    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    napi_set_named_property(env, obj, "width", make_int(env, static_cast<int>(icon.width)));
    napi_set_named_property(env, obj, "height", make_int(env, static_cast<int>(icon.height)));

    void *buffer_data = nullptr;
    napi_value array_buffer = nullptr;
    napi_create_arraybuffer(env, icon.rgba.size(), &buffer_data, &array_buffer);
    std::memcpy(buffer_data, icon.rgba.data(), icon.rgba.size());
    napi_set_named_property(env, obj, "data", array_buffer);

    if (!icon.mask_rgba.empty()) {
        void *mask_data = nullptr;
        napi_value mask_buffer = nullptr;
        napi_create_arraybuffer(env, icon.mask_rgba.size(), &mask_data, &mask_buffer);
        std::memcpy(mask_data, icon.mask_rgba.data(), icon.mask_rgba.size());
        napi_set_named_property(env, obj, "maskWidth", make_int(env, static_cast<int>(icon.mask_width)));
        napi_set_named_property(env, obj, "maskHeight", make_int(env, static_cast<int>(icon.mask_height)));
        napi_set_named_property(env, obj, "maskData", mask_buffer);
    }

    return obj;
}

static napi_value GetAppIcon(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_undefined(env);
    }
    const std::uint32_t uid = static_cast<std::uint32_t>(get_int64(env, args[0]));
    return icon_to_js(env, launcher()->get_app_icon(uid));
}

static napi_value LaunchApp(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->launch_app(static_cast<std::uint32_t>(get_int(env, args[0])));
    }
    return make_undefined(env);
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

static napi_value GetDevices(napi_env env, napi_callback_info info) {
    return launcher() ? make_string_array(env, launcher()->get_devices()) : make_string_array(env, {});
}

static napi_value GetDeviceFirmwareCodes(napi_env env, napi_callback_info info) {
    return launcher() ? make_string_array(env, launcher()->get_device_firwmare_codes()) : make_string_array(env, {});
}

static napi_value SetCurrentDevice(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->set_current_device(get_int(env, args[0]), get_bool(env, args[1]));
    }
    return make_undefined(env);
}

static napi_value SetDeviceName(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        const std::string name = get_string(env, args[1]);
        launcher()->set_device_name(get_int(env, args[0]), name.c_str());
    }
    return make_undefined(env);
}

static napi_value RescanDevices(napi_env env, napi_callback_info info) {
    if (launcher()) {
        launcher()->rescan_devices();
    }
    return make_undefined(env);
}

static napi_value GetCurrentDevice(napi_env env, napi_callback_info info) {
    return make_int(env, launcher() ? static_cast<int>(launcher()->get_current_device()) : 0);
}

static napi_value InstallDevice(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_int(env, -1);
    }
    std::string rpkg_path = get_string(env, args[0]);
    std::string rom_path = get_string(env, args[1]);
    const bool install_rpkg = get_bool(env, args[2]);
    return make_int(env, static_cast<int>(launcher()->install_device(rpkg_path, rom_path, install_rpkg)));
}

static napi_value DoesRomNeedRPKG(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_bool(env, false);
    }
    return make_bool(env, launcher()->does_rom_need_rpkg(get_string(env, args[0])));
}

// ---------------------------------------------------------------------------
// Packages / installs
// ---------------------------------------------------------------------------

static napi_value InstallApp(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_int(env, -1);
    }
    std::string path = get_string(env, args[0]);
    return make_int(env, static_cast<int>(launcher()->install_app(path)));
}

static napi_value GetPackages(napi_env env, napi_callback_info info) {
    return launcher() ? make_string_array(env, launcher()->get_packages()) : make_string_array(env, {});
}

static napi_value UninstallPackage(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->uninstall_package(get_int(env, args[0]), get_int(env, args[1]));
    }
    return make_undefined(env);
}

static napi_value MountSdCard(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        std::string path = get_string(env, args[0]);
        launcher()->mount_sd_card(path);
    }
    return make_undefined(env);
}

static napi_value InstallNGageGame(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_int(env, -1);
    }
    std::string path = get_string(env, args[0]);
    return make_int(env, launcher()->install_ngage_game(path));
}

// ---------------------------------------------------------------------------
// Config / languages / settings
// ---------------------------------------------------------------------------

static napi_value LoadConfig(napi_env env, napi_callback_info info) {
    if (launcher()) {
        launcher()->load_config();
    }
    return make_undefined(env);
}

static napi_value SetLanguage(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->set_language(get_int(env, args[0]));
    }
    return make_undefined(env);
}

static napi_value SetRtosLevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->set_rtos_level(get_int(env, args[0]));
    }
    return make_undefined(env);
}

static napi_value UpdateAppSetting(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->update_app_setting(static_cast<std::uint32_t>(get_int(env, args[0])));
    }
    return make_undefined(env);
}

static napi_value GetLanguageIds(napi_env env, napi_callback_info info) {
    return launcher() ? make_string_array(env, launcher()->get_language_ids()) : make_string_array(env, {});
}

static napi_value GetLanguageNames(napi_env env, napi_callback_info info) {
    return launcher() ? make_string_array(env, launcher()->get_language_names()) : make_string_array(env, {});
}

static napi_value SetScreenParams(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value args[7] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        std::string bg_path = get_string(env, args[4]);
        launcher()->set_screen_params(
            static_cast<std::uint32_t>(get_int(env, args[0])),
            static_cast<std::uint32_t>(get_int(env, args[1])),
            static_cast<std::uint32_t>(get_int(env, args[2])),
            static_cast<std::uint32_t>(get_int(env, args[3])),
            bg_path,
            static_cast<float>(get_double(env, args[5])),
            get_bool(env, args[6]));
    }
    return make_undefined(env);
}

// ---------------------------------------------------------------------------
// Runtime stats / dialogs / misc
// ---------------------------------------------------------------------------

static napi_value GetFps(napi_env env, napi_callback_info info) {
    return make_double(env, g_state ? g_state->present_fps.load() : 0.0f);
}

static napi_value IsFpsCounterEnabled(napi_env env, napi_callback_info info) {
    return make_bool(env, g_state ? g_state->conf.show_fps : false);
}

static napi_value SubmitInput(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->on_finished_text_input(get_string(env, args[0]), false);
    }
    return make_undefined(env);
}

static napi_value SubmitQuestionDialogResponse(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->on_question_dialog_finished(get_int(env, args[0]));
    }
    return make_undefined(env);
}

static napi_value SaveScreenshotTo(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!launcher()) {
        return make_bool(env, false);
    }
    return make_bool(env, launcher()->save_screenshot_to(get_string(env, args[0])));
}

static napi_value SetCurrentMMCID(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (launcher()) {
        launcher()->set_current_mmc_id(get_string(env, args[0]));
    }
    return make_undefined(env);
}

// ---------------------------------------------------------------------------
// Input (called from ArkUI touch/key handlers as a fallback to XComponent)
// ---------------------------------------------------------------------------

static napi_value PressKey(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (g_state && g_state->winserv) {
        eka2l1::hos::press_key(*g_state, get_int(env, args[0]), get_int(env, args[1]));
    }
    return make_undefined(env);
}

static napi_value TouchScreen(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (g_state && g_state->winserv) {
        eka2l1::hos::touch_screen(*g_state, get_int(env, args[0]), get_int(env, args[1]),
            get_int(env, args[2]), get_int(env, args[3]), get_int(env, args[4]));
    }
    return make_undefined(env);
}

// ---------------------------------------------------------------------------
// XComponent surface lifecycle (driven by xcomponent_bridge, which isolates the
// XComponent SDK header from EKA2L1's colliding keycode.inc).
// ---------------------------------------------------------------------------

static void surface_created(void *window, int width, int height) {
    if (!g_state) {
        return;
    }

    g_surface = window;
    g_state->window->surface_changed(window, width, height);

    if (!g_threads_inited) {
        eka2l1::hos::init_threads(*g_state);
        g_threads_inited = true;
    } else {
        eka2l1::hos::start_threads(*g_state);
    }
}

static void surface_changed(void *window, int width, int height) {
    if (g_state) {
        g_state->window->surface_changed(window, width, height);
    }
}

static void surface_destroyed(void *window) {
    if (!g_state) {
        return;
    }

    eka2l1::hos::pause_threads(*g_state);
    g_state->window->surface_changed(nullptr, 0, 0);
    g_surface = nullptr;
}

static void surface_touch(int x, int y, int action, int pointer_id) {
    if (g_state && g_state->winserv) {
        eka2l1::hos::touch_screen(*g_state, x, y, 0, action, pointer_id);
    }
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "setDirectory", nullptr, SetDirectory, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "startNative", nullptr, StartNative, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isInitialized", nullptr, IsInitialized, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getApps", nullptr, GetApps, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getAppIcon", nullptr, GetAppIcon, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "launchApp", nullptr, LaunchApp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getDevices", nullptr, GetDevices, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getDeviceFirmwareCodes", nullptr, GetDeviceFirmwareCodes, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setCurrentDevice", nullptr, SetCurrentDevice, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setDeviceName", nullptr, SetDeviceName, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "rescanDevices", nullptr, RescanDevices, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getCurrentDevice", nullptr, GetCurrentDevice, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "installDevice", nullptr, InstallDevice, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "doesRomNeedRPKG", nullptr, DoesRomNeedRPKG, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "installApp", nullptr, InstallApp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getPackages", nullptr, GetPackages, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "uninstallPackage", nullptr, UninstallPackage, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "mountSdCard", nullptr, MountSdCard, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "installNGageGame", nullptr, InstallNGageGame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "loadConfig", nullptr, LoadConfig, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setLanguage", nullptr, SetLanguage, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRtosLevel", nullptr, SetRtosLevel, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "updateAppSetting", nullptr, UpdateAppSetting, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getLanguageIds", nullptr, GetLanguageIds, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getLanguageNames", nullptr, GetLanguageNames, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setScreenParams", nullptr, SetScreenParams, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getFps", nullptr, GetFps, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isFpsCounterEnabled", nullptr, IsFpsCounterEnabled, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "submitInput", nullptr, SubmitInput, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "submitQuestionDialogResponse", nullptr, SubmitQuestionDialogResponse, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "saveScreenshotTo", nullptr, SaveScreenshotTo, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setCurrentMMCID", nullptr, SetCurrentMMCID, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pressKey", nullptr, PressKey, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "touchScreen", nullptr, TouchScreen, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // Hook the XComponent so the run page's <XComponent> surface drives rendering.
    eka2l1::hos::xcbridge::surface_handlers handlers;
    handlers.on_created = surface_created;
    handlers.on_changed = surface_changed;
    handlers.on_destroyed = surface_destroyed;
    handlers.on_touch = surface_touch;
    eka2l1::hos::xcbridge::register_xcomponent(env, exports, handlers);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&demoModule);
}
