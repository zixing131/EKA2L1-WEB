#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/kernel.h>
#include <kernel/property.h>
#include <kernel/timing.h>

#include <cstring>
#include <memory>
#include <new>

TEST_CASE("integer properties start at zero on reused storage", "[property]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);

    // The Calculator reads Avkon's QWERTY-mode property before anyone sets it.
    // Poison allocator storage so this cannot pass merely because malloc returned
    // a fresh zero-filled page. Also exercise reuse after a previous nonzero value.
    alignas(service::property) unsigned char storage[sizeof(service::property)];
    std::memset(storage, 0xA5, sizeof(storage));
    for (int generation = 0; generation < 2; ++generation) {
        auto destroy = [](service::property *prop) { prop->~property(); };
        std::unique_ptr<service::property, decltype(destroy)> prop(
            new (storage) service::property(&kernel), destroy);
        REQUIRE_FALSE(prop->is_defined());
        prop->define(service::property_type::int_data, 0);
        REQUIRE(prop->get_int() == 0);
        REQUIRE(prop->set_int(1));
        REQUIRE(prop->get_int() == 1);
    }
}
