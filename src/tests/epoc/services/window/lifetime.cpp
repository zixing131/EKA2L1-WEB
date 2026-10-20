// Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later.
#include <catch2/catch.hpp>
#include <services/window/classes/winbase.h>
#include <memory>

TEST_CASE("window destruction unlinks children before the parent is destroyed", "[window-lifetime]") {
    using eka2l1::epoc::window;
    auto parent = std::make_unique<window>(nullptr, nullptr, nullptr);
    auto oldest = std::make_unique<window>(nullptr, nullptr, parent.get());
    auto newest = std::make_unique<window>(nullptr, nullptr, parent.get());
    REQUIRE(parent->child == newest.get());
    oldest.reset();
    REQUIRE(newest->sibling == nullptr);
    newest.reset();
    REQUIRE(parent->child == nullptr);
    parent.reset(); // Previously writes through the freed owned top_canvas.
}

TEST_CASE("children can outlive a destroyed window parent", "[window-lifetime]") {
    using eka2l1::epoc::window;
    auto parent = std::make_unique<window>(nullptr, nullptr, nullptr);
    auto child = std::make_unique<window>(nullptr, nullptr, parent.get());
    parent.reset();
    REQUIRE(child->parent == nullptr);
}
