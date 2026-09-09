#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/kernel.h>
#include <kernel/server.h>
#include <kernel/timing.h>

namespace eka2l1::epoc {
    void message_construct_from_ptr(kernel_system *, kernel::handle, service::message2 *);
}

TEST_CASE("RMessagePtr2 reconstructs the original request without overwriting user fields", "[ipc-message]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    kernel_system kernel(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    auto *request = kernel.create_msg(kernel::owner_type::thread);
    REQUIRE(request);
    request->function = 0x123;
    request->args = ipc_arg(7, 8, 9, 10, 0);
    request->session_ptr_lle = 0x123456;
    service::message2 result{};
    result.flags = 0x11223344;
    result.spare3 = 0x556677;
    epoc::message_construct_from_ptr(&kernel, request->id, &result);
    CHECK(result.ipc_msg_handle == request->id);
    CHECK(result.function == 0x123);
    for (int i = 0; i < 4; ++i) CHECK(result.args[i] == 7 + i);
    CHECK(result.session_ptr == 0x123456);
    CHECK(result.spare1 == 0);
    CHECK(result.flags == 0x11223344);
    CHECK(result.spare3 == 0x556677);
    request->function = -2; // RMessage2::EDisConnect
    epoc::message_construct_from_ptr(&kernel, request->id, &result);
    CHECK(result.function == -2);
    for (int arg : result.args) CHECK(arg == 0);
}
