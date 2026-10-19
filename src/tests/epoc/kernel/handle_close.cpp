#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/kernel.h>
#include <kernel/object_ix.h>
#include <kernel/property.h>
#include <kernel/timing.h>

TEST_CASE("closing an ordinary object's last handle does not request DLL destructors", "[kernel][handle]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    kernel::object_ix handles(&kernel, kernel::handle_array_owner::process);
    auto *prop = kernel.create<service::property>();
    const auto first = handles.add_object(prop);
    const auto second = handles.add_object(prop);
    // Drop the creator's reference so the last handle actually destroys it.
    REQUIRE(prop->decrease_access_count() == 0);
    REQUIRE(handles.close(first) == 0);
    REQUIRE(handles.close(second) == 0);
    REQUIRE(handles.close(second) == -1);

    // Direct destruction still reports removal separately from guest Close.
    auto *other = kernel.create<service::property>();
    int close_result = -1;
    REQUIRE(kernel.destroy(other, &close_result));
    REQUIRE(close_result == 0);
}
