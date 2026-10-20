#include <catch2/catch.hpp>
#include <cpu/dyncom/arm_dyncom.h>
#include <cpu/dyncom/armstate.h>
#include <cpu/12l1r/exclusive_monitor.h>
#include <array>
#include <cstring>

TEST_CASE("Dyncom keeps same-PC translations distinct across address spaces", "[dyncom]") {
    using namespace eka2l1;
    arm::r12l1::exclusive_monitor monitor(1);
    arm::dyncom_core core(&monitor, 12);
    std::uint32_t address_space = 1;
    core.read_code = [&](arm::address pc, std::uint32_t *value) {
        *value = pc == 0x1000 ? (0xE3A00000 | address_space) : 0xE12FFF1E; // MOV r0,#asid; BX lr
        return true;
    };
    for (const auto asid : {1u, 2u, 1u}) {
        address_space = asid;
        core.flush_tlb();
        core.set_asid(asid);
        core.set_cpsr(0x10);
        core.set_pc(0x1000);
        core.set_lr(0x2000);
        core.run(2);
        CHECK(core.get_reg(0) == asid);
        CHECK(core.get_pc() == 0x2000);
    }
}

TEST_CASE("Dyncom resolves a split word through the MMU instead of overrunning a cached page", "[dyncom]") {
    using namespace eka2l1;
    arm::r12l1::exclusive_monitor monitor(1);
    arm::dyncom_core core(&monitor, 12);
    auto state = std::make_unique<ARMul_State>(&core, USER32MODE);
    std::array<std::uint8_t, 4096 + 4> first{}, second{};
    first.fill(0xCC);
    first[4094] = 0x11; first[4095] = 0x22;
    second[0] = 0x33; second[1] = 0x44;
    core.set_tlb_page(0x1000, first.data(), prot_read_write);
    core.set_tlb_page(0x2000, second.data(), prot_read_write);
    unsigned reads = 0, writes = 0;
    core.read_32bit = [&](arm::address at, std::uint32_t *value) {
        REQUIRE(at == 0x1FFE);
        ++reads;
        *value = first[4094] | (first[4095] << 8) | (second[0] << 16) | (second[1] << 24);
        return true;
    };
    core.write_32bit = [&](arm::address at, std::uint32_t *value) {
        REQUIRE(at == 0x1FFE);
        ++writes;
        first[4094] = *value; first[4095] = *value >> 8;
        second[0] = *value >> 16; second[1] = *value >> 24;
        return true;
    };
    CHECK(state->ReadMemory32(0x1FFE) == 0x44332211);
    state->WriteMemory32(0x1FFE, 0x88776655);
    CHECK(reads == 1);
    CHECK(writes == 1);
    CHECK(first[4096] == 0xCC);
    CHECK(second[0] == 0x77);
    CHECK(second[1] == 0x88);
}
