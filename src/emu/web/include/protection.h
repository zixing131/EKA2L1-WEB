/*
 * Copyright (c) 2026 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project - WebAssembly port.
 *
 * Copyright notice / build watermark for the web build. The project is
 * licensed under the GNU Affero General Public License v3; there is no
 * domain whitelist or integrity gating.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eka2l1::web::protection {
    // Multi-line copyright notice (baked into the wasm, referenced so the
    // linker can't strip it).
    std::string copyright_text();

    // Multi-line build info: product, build timestamp, git commit, channel.
    std::string build_info();

    // Single compact line for the on-screen watermark
    // ("Build: <ts>  <commit>  [<channel>]").
    std::string watermark_text();

    // Render ASCII text into a tightly-sized RGBA8888 buffer using the built-in
    // 8x8 font. Set pixels are white with a fixed translucent alpha; gaps are
    // fully transparent. `scale` is an integer pixel multiplier. Returns false
    // for empty text. Used by the frontend to build the watermark texture.
    bool render_text_rgba(const std::string &text, int scale,
        std::vector<std::uint8_t> &out, int &width, int &height);
}
