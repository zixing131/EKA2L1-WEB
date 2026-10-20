#include <catch2/catch.hpp>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/chunk.h>
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/thread.h>
#include <kernel/timing.h>
#include <mem/mem.h>
#include <services/sms/protocols/datagram.h>
#include <services/socket/socket.h>
#include <utils/err.h>

TEST_CASE("unsupported socket operations complete the guest request", "[socket][completion]") {
    using namespace eka2l1;
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    memory_system memory(monitor.get(), &conf, mem::mem_model_type::multiple, false);
    kernel_system kern(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    kern.install_memory(&memory);
    kern.set_epoc_version(epocver::epoc94);
    auto *process = kern.create<kernel::process>(&memory, "socket-test", u"socket-test.exe", u"");
    auto *thread = kern.create<kernel::thread>(&memory, &timing, process,
        kernel::access_type::local_access, "SocketTest", 0, 0x4000, 0, 0,
        true, 0, 0, kernel::priority_normal);
    auto *chunk = kern.create<kernel::chunk>(&memory, process, "", 0, 0x1000, 0x1000,
        prot_read_write, kernel::chunk_type::normal, kernel::chunk_access::local, kernel::chunk_attrib::none);
    ptr<epoc::request_status> status_ptr(chunk->base(process).ptr_address());
    auto *status = status_ptr.get(process);
    REQUIRE(status);
    new (status) epoc::request_status(epoc::request_status::pending_status, false);
    epoc::notify_info info(status_ptr, thread);
    const int signals_before = thread->request_count();
    epoc::socket::socket socket;

    SECTION("receive") {
        socket.receive(nullptr, 0, nullptr, nullptr, 0, info, {});
    }
    SECTION("SMS parameter read uses the unsupported receive fallback") {
        // J9 WMA reads SIM SMS parameters while initializing Anyview. SMS has
        // no receive implementation; its inherited request must not hang.
        epoc::sms::datagram_socket sms(nullptr);
        sms.receive(nullptr, 0, nullptr, nullptr, 0, info, {});
    }
    SECTION("send") {
        socket.send(nullptr, 0, nullptr, nullptr, 0, info);
    }
    SECTION("ioctl") {
        socket.ioctl(0, info, nullptr, 0, 0, 0);
    }
    SECTION("shutdown") {
        socket.shutdown(info, 0);
    }
    CHECK(status->status == epoc::error_not_supported);
    CHECK((status->flags & epoc::request_status::pending) == 0);
    CHECK(info.empty());
    CHECK(thread->request_count() == signals_before + 1);
}
