/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project - WebAssembly port
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
#include <common/cvt.h>
#include <unordered_map>
#include <drivers/ui/input_dialog.h>

#include <emscripten.h>

namespace {
    std::uint32_t next_question = 0;
    std::unordered_map<std::uint32_t, eka2l1::drivers::ui::yes_no_dialog_complete_callback> questions;
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_answer_question(std::uint32_t id, int choice) {
    const auto found = questions.find(id);
    if (found == questions.end()) return;
    auto callback = std::move(found->second);
    questions.erase(found);
    if (callback) callback(choice);
}

EM_JS(void, web_show_question, (std::uint32_t id, const char *message, const char *first, const char *second), {
    if (!Module.ekaQuestions) Module.ekaQuestions = [];
    const queue = Module.ekaQuestions;
    const request = { id: id, message: UTF8ToString(message), labels: [UTF8ToString(first), UTF8ToString(second)] };
    const show = function() {
        const dialog = document.createElement('dialog');
        dialog.id = 'ekaSystemQuestion';
        dialog.setAttribute('aria-label', 'System confirmation');
        dialog.style.cssText = 'max-width:min(420px,85vw);border:1px solid #888;border-radius:16px;padding:24px;color:#eee;background:#20242b;box-shadow:0 16px 80px #0008;font:16px/1.5 system-ui;z-index:2147483647';
        const text = document.createElement('p');
        text.style.cssText = 'white-space:pre-wrap;overflow-wrap:anywhere;margin:0 0 24px';
        text.textContent = request.message;
        dialog.appendChild(text);
        const actions = document.createElement('div');
        actions.style.cssText = 'display:flex;justify-content:flex-end;gap:12px';
        dialog.appendChild(actions);
        const previousFocus = document.activeElement;
        let answered = false;
        const finish = function(choice) {
            if (answered) return;
            answered = true;
            dialog.close();
            dialog.remove();
            window.removeEventListener('keydown', captureKey, true);
            window.removeEventListener('keyup', captureKey, true);
            queue.shift();
            if (previousFocus && previousFocus.isConnected) previousFocus.focus();
            Module._wasm_answer_question(request.id, choice);
            if (queue.length) queue[0]();
        };
        const captureKey = function(event) {
            // A dialog key must never also press a guest phone key.
            event.stopImmediatePropagation();
            if (event.key === 'Escape') {
                event.preventDefault();
                if (event.type === 'keydown') finish(-1);
            }
        };
        request.labels.forEach(function(label, index) {
            const button = document.createElement('button');
            button.type = 'button';
            button.dataset.choice = String(index);
            button.textContent = label;
            button.style.cssText = 'font:inherit;color:#fff;background:#36404d;border:1px solid #78828e;border-radius:8px;padding:8px 24px;cursor:pointer';
            button.onclick = function() { finish(index); };
            actions.appendChild(button);
        });
        dialog.addEventListener('cancel', function(event) { event.preventDefault(); finish(-1); });
        document.body.appendChild(dialog);
        window.addEventListener('keydown', captureKey, true);
        window.addEventListener('keyup', captureKey, true);
        dialog.showModal();
        actions.lastChild.focus();
    };
    queue.push(show);
    if (queue.length === 1) show();
});

namespace eka2l1::common {
    bool launch_browser(const std::string &url) {
        std::string js = "window.open('" + url + "', '_blank');";
        emscripten_run_script(js.c_str());
        return true;
    }
}

namespace eka2l1::drivers::ui {
    bool open_input_view(const std::u16string &initial_text, const int max_len, input_dialog_complete_callback complete_callback) {
        // No-op stub: Web input is handled via JS/HTML overlay
        return false;
    }

    void close_input_view() {
        // No-op stub
    }

    void show_yes_no_dialog(const std::u16string &text, const std::u16string &button1_text,
        const std::u16string &button2_text, yes_no_dialog_complete_callback complete_callback) {
        const auto id = ++next_question;
        questions.emplace(id, std::move(complete_callback));
        const auto message = common::ucs2_to_utf8(text);
        const auto first = common::ucs2_to_utf8(button1_text);
        const auto second = common::ucs2_to_utf8(button2_text);
        web_show_question(id, message.c_str(), first.c_str(), second.c_str());
    }
}
