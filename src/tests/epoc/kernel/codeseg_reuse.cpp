#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/codeseg.h>
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/timing.h>
#include <mem/mem.h>

TEST_CASE("shared code must not import a process-local DLL", "[kernel][codeseg]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    memory_system memory(monitor.get(), &conf, mem::mem_model_type::multiple, false);
    kernel_system kern(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    kern.install_memory(&memory);
    kern.set_epoc_version(epocver::epoc94);
    std::uint8_t code[16]{};
    auto make_segment = [&](const char *name, std::uint32_t bss) {
        kernel::codeseg_create_info info{};
        info.code_data = code;
        info.code_size = sizeof(code);
        info.text_size = sizeof(code);
        info.bss_size = bss;
        info.export_table = {4};
        return kern.create<kernel::codeseg>(name, info);
    };
    auto *local = make_segment("local.dll", 4);
    auto *client = make_segment("client.dll", 0);
    REQUIRE(client->add_dependency({local, {kernel::make_import_info(0, 1)}}));

    // Prior eligibility/attachment walks must not hide a stateful dependency.
    CHECK_FALSE(local->eligible_for_codeseg_reuse());
    CHECK_FALSE(client->eligible_for_codeseg_reuse());
    CHECK_FALSE(client->eligible_for_codeseg_reuse());

    auto *first = kern.create<kernel::process>(&memory, "first", u"first.exe", u"");
    auto *second = kern.create<kernel::process>(&memory, "second", u"second.exe", u"");
    client->unmark();
    REQUIRE(client->attach(first));
    client->unmark();
    REQUIRE(client->attach(second));
    std::uint8_t *first_code = nullptr, *second_code = nullptr;
    const auto first_addr = client->get_code_run_addr(first, &first_code);
    const auto second_addr = client->get_code_run_addr(second, &second_code);
    REQUIRE(first_code);
    REQUIRE(second_code);
    CHECK(first_addr != second_addr);
    CHECK(*reinterpret_cast<std::uint32_t *>(first_code) == local->lookup(first, 1));
    CHECK(*reinterpret_cast<std::uint32_t *>(second_code) == local->lookup(second, 1));
    CHECK(local->lookup(first, 1) != local->lookup(second, 1));

    auto *pure = make_segment("pure.dll", 0);
    CHECK(pure->eligible_for_codeseg_reuse());
    CHECK(pure->eligible_for_codeseg_reuse());
}
