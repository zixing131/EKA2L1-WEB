#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/kernel.h>
#include <kernel/property.h>
#include <kernel/timing.h>
#include <utils/err.h>

#include <cstring>
#include <memory>
#include <new>

namespace eka2l1::epoc {
    std::int32_t property_find_set_int(kernel_system *, std::int32_t, std::int32_t, std::int32_t);
}

TEST_CASE("binary property reads return stored length and preserve the unused destination", "[property]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    auto prop = kernel.create<service::property>();
    std::uint8_t output[16];
    std::memset(output, 0xCC, sizeof(output));
    CHECK(prop->read_bin(output, sizeof(output)) == epoc::error_not_found);
    prop->define(service::property_type::bin_data, 0);
    CHECK(prop->read_bin(output, sizeof(output)) == 0);
    CHECK(output[0] == 0xCC);
    std::uint8_t value[]{1, 2, 3};
    REQUIRE(prop->set(value, sizeof(value)));
    CHECK(prop->read_bin(output, sizeof(output)) == 3);
    CHECK(std::memcmp(output, value, sizeof(value)) == 0);
    CHECK(output[3] == 0xCC);
    output[0] = output[1] = output[2] = 0xCC;
    CHECK(prop->read_bin(output, 2) == epoc::error_overflow);
    CHECK(output[0] == 1);
    CHECK(output[1] == 2);
    CHECK(output[2] == 0xCC);
    CHECK(prop->read_bin(output, -1) == epoc::error_argument);
    prop->define(service::property_type::int_data, 0);
    CHECK(prop->read_bin(output, sizeof(output)) == epoc::error_argument);
}

TEST_CASE("category integer property writes update the value seen by subscribers", "[property]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    auto prop = kernel.create<service::property>();
    prop->first = 0x100058F4;
    prop->second = 1;
    prop->define(service::property_type::int_data, 0);
    int observed = -1;
    prop->add_data_change_callback(&observed, [](void *data, service::property *changed) {
        *static_cast<int *>(data) = changed->get_int();
    });
    REQUIRE(epoc::property_find_set_int(&kernel, prop->first, prop->second, 1) == epoc::error_none);
    REQUIRE(prop->get_int() == 1);
    REQUIRE(observed == 1);
    REQUIRE(epoc::property_find_set_int(&kernel, prop->first, prop->second, 3) == epoc::error_none);
    REQUIRE(prop->get_int() == 3);
    REQUIRE(observed == 3);
    REQUIRE(epoc::property_find_set_int(&kernel, prop->first, 2, 1) == epoc::error_not_found);
    prop->define(service::property_type::bin_data, 4);
    REQUIRE(epoc::property_find_set_int(&kernel, prop->first, prop->second, 1) == epoc::error_argument);
}

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
