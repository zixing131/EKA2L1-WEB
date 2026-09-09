#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/kernel.h>
#include <kernel/timing.h>
#include <services/etel/modmngr.h>
#include <services/etel/phone.h>

TEST_CASE("ETel enumerates phones independently of line entry order", "[etel]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    epoc::etel::module_manager manager;
    REQUIRE(manager.load_tsy(&kernel, nullptr, 1, "phonetsy"));
    REQUIRE(manager.total_entries(epoc::etel_entry_phone) == 1);
    REQUIRE(manager.total_entries(epoc::etel_entry_line) == 4);
    const auto index = manager.get_entry_real_index(0, epoc::etel_entry_phone);
    REQUIRE(index.has_value());
    epoc::etel_module_entry *entry = nullptr;
    REQUIRE(manager.get_entry(*index, &entry));
    CHECK(entry->entity_->type() == epoc::etel_entry_phone);
    CHECK(static_cast<etel_phone *>(entry->entity_.get())->lines_.size() == 4);
    CHECK_FALSE(manager.get_entry_real_index(1, epoc::etel_entry_phone).has_value());
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto line = manager.get_entry_real_index(i, epoc::etel_entry_line);
        REQUIRE(line.has_value());
        REQUIRE(manager.get_entry(*line, &entry));
        CHECK(entry->entity_->type() == epoc::etel_entry_line);
    }
    CHECK_FALSE(manager.get_entry_real_index(4, epoc::etel_entry_line).has_value());
}
