// Cpu tests, using plain <cassert>: a failed check aborts the program, which is all a test
// needs to do here. No framework, no dependency.
//
// Two halves: the register file (day 3) and fetch-decode-execute (day 4). The instruction
// tests run real machine code out of a synthetic 32 KiB cartridge, so they exercise the
// same path a real ROM does: PC -> Bus -> Rom, not a special test hook.
//
// Build: cmake --build --preset default
// Run:   ./build/cpu_tests.exe      (or: ctest --test-dir build --output-on-failure)

// assert() disappears when NDEBUG is defined (Release builds). Undefine it before <cassert>
// so the checks still run if this is ever built optimised.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../src/bus.hpp"
#include "../src/cpu.hpp"

namespace {

void pair_accessors_test() {
    Cpu cpu;

    // A 16-bit write splits into two bytes: high byte into B, low byte into C.
    cpu.set_bc(0x1234);
    assert(cpu.b() == 0x12);
    assert(cpu.c() == 0x34);
    assert(cpu.bc() == 0x1234);

    // ...and writing a byte goes the other way: bc() recombines it.
    cpu.set_c(0xAB);
    assert(cpu.b() == 0x12);
    assert(cpu.c() == 0xAB);
    assert(cpu.bc() == 0x12AB);

    // Same for the other pairs, high byte first.
    cpu.set_de(0xDEAD);
    assert(cpu.d() == 0xDE);
    assert(cpu.e() == 0xAD);
    assert(cpu.de() == 0xDEAD);

    cpu.set_hl(0xBEEF);
    assert(cpu.h() == 0xBE);
    assert(cpu.l() == 0xEF);
    assert(cpu.hl() == 0xBEEF);

    cpu.set_h(0x12);
    assert(cpu.hl() == 0x12EF);
}

void af_masking_test() {
    Cpu cpu;

    // F has only a high nibble; the low four bits of AF must never survive.
    cpu.set_af(0xFFFF);
    assert(cpu.a() == 0xFF);
    assert(cpu.f() == 0xF0);
    assert(cpu.af() == 0xFFF0);

    // Writing F directly is masked the same way, so there is no back door.
    cpu.set_f(0xFF);
    assert(cpu.f() == 0xF0);
    cpu.set_f(0x0F);
    assert(cpu.f() == 0x00);

    // A normal value is preserved bit for bit.
    cpu.set_af(0x01B0);
    assert(cpu.a() == 0x01);
    assert(cpu.f() == 0xB0);
    assert(cpu.af() == 0x01B0);
}

void flags_test() {
    Cpu cpu;

    // Setting one flag touches only that bit.
    cpu.set_f(0x00);
    cpu.set_n_flag(true);
    assert(cpu.f() == Cpu::FLAG_N);
    assert(cpu.n_flag());
    assert(!cpu.z_flag() && !cpu.h_flag() && !cpu.c_flag());

    cpu.set_f(0x00);
    cpu.set_h_flag(true);
    assert(cpu.f() == Cpu::FLAG_H);
    assert(cpu.h_flag());

    cpu.set_f(0x00);
    cpu.set_c_flag(true);
    assert(cpu.f() == Cpu::FLAG_C);
    assert(cpu.c_flag());

    cpu.set_f(0x00);
    cpu.set_z_flag(true);
    assert(cpu.f() == Cpu::FLAG_Z);
    assert(cpu.z_flag());

    // Clearing one flag leaves the other three alone.
    cpu.set_f(0xF0);
    cpu.set_z_flag(false);
    assert(cpu.f() == 0x70);
    assert(!cpu.z_flag() && cpu.n_flag() && cpu.h_flag() && cpu.c_flag());

    cpu.set_z_flag(true);
    cpu.set_n_flag(false);
    assert(cpu.f() == 0xB0);
    assert(cpu.z_flag() && !cpu.n_flag() && cpu.h_flag() && cpu.c_flag());

    cpu.set_h_flag(false);
    assert(cpu.f() == 0x90);
    assert(cpu.z_flag() && !cpu.n_flag() && !cpu.h_flag() && cpu.c_flag());

    cpu.set_c_flag(false);
    assert(cpu.f() == 0x80);
    assert(cpu.z_flag() && !cpu.n_flag() && !cpu.h_flag() && !cpu.c_flag());

    // Setting a flag that is already set is a no-op, and vice versa.
    cpu.set_z_flag(true);
    assert(cpu.f() == 0x80);
    cpu.set_n_flag(false);
    assert(cpu.f() == 0x80);

    // The masks are the documented bit values.
    assert(Cpu::FLAG_Z == 0x80);
    assert(Cpu::FLAG_N == 0x40);
    assert(Cpu::FLAG_H == 0x20);
    assert(Cpu::FLAG_C == 0x10);
}

void reset_test() {
    Cpu cpu;

    // Scramble everything, then reset and compare against the Pan Docs DMG post-boot table.
    cpu.set_af(0x1234);
    cpu.set_bc(0x5678);
    cpu.set_de(0x9ABC);
    cpu.set_hl(0xDEF0);
    cpu.set_sp(0x1111);
    cpu.set_pc(0x2222);

    cpu.reset();

    assert(cpu.a() == 0x01);
    assert(cpu.f() == 0xB0); // Z=1, N=0, H=1, C=1
    assert(cpu.af() == 0x01B0);

    assert(cpu.b() == 0x00);
    assert(cpu.c() == 0x13);
    assert(cpu.bc() == 0x0013);

    assert(cpu.d() == 0x00);
    assert(cpu.e() == 0xD8);
    assert(cpu.de() == 0x00D8);

    assert(cpu.h() == 0x01);
    assert(cpu.l() == 0x4D);
    assert(cpu.hl() == 0x014D);

    assert(cpu.sp() == 0xFFFE);
    assert(cpu.pc() == 0x0100);

    // A freshly constructed CPU is already reset, not full of zeros or garbage.
    const Cpu fresh;
    assert(fresh.af() == Cpu::POST_BOOT_AF);
    assert(fresh.bc() == Cpu::POST_BOOT_BC);
    assert(fresh.de() == Cpu::POST_BOOT_DE);
    assert(fresh.hl() == Cpu::POST_BOOT_HL);
    assert(fresh.sp() == Cpu::POST_BOOT_SP);
    assert(fresh.pc() == Cpu::POST_BOOT_PC);

    assert(fresh.z_flag() && !fresh.n_flag() && fresh.h_flag() && fresh.c_flag());
}

// ---------------------------------------------------------------------------------------
// Day 4: fetch / decode / execute
// ---------------------------------------------------------------------------------------

// A whole little machine: cartridge image -> bus -> CPU, with PC on the entry point.
// `code` is placed at 0x0100, which is where a post-boot CPU starts reading.
struct Machine {
    explicit Machine(const std::vector<uint8_t>& code, uint16_t origin = 0x0100)
        : rom(make_image(code, origin)), bus(), cpu() {
        bus.attach_rom(&rom);
        cpu.attach_bus(&bus);
        cpu.reset();
    }

    // 32 KiB, zero-filled: the header decode then reads "no MBC, 32 KiB, no RAM", which
    // is a legal cartridge and needs no hand-written header bytes.
    static Rom make_image(const std::vector<uint8_t>& code, uint16_t origin) {
        std::vector<uint8_t> bytes(0x8000, 0x00);
        for (size_t i = 0; i < code.size(); ++i) {
            bytes[static_cast<size_t>(origin) + i] = code[i];
        }
        return Rom(std::move(bytes));
    }

    Rom rom;
    Bus bus;
    Cpu cpu;
};

void step_without_bus_test() {
    // A Cpu is a valid register file with no bus; only step() needs memory.
    Cpu cpu;
    bool threw = false;
    try {
        cpu.step();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void nop_test() {
    Machine m({0x00});

    assert(m.cpu.pc() == 0x0100);
    assert(m.cpu.step() == 4); // 4 T-cycles: one opcode fetch, nothing else
    assert(m.cpu.pc() == 0x0101);

    // NOP touches nothing at all — not even the flags it was handed at boot.
    assert(m.cpu.af() == Cpu::POST_BOOT_AF);
}

void ld_r8_imm_and_reg_test() {
    Machine m({0x3E, 0x42,   // LD A,0x42
               0x06, 0x12,   // LD B,0x12
               0x0E, 0x34,   // LD C,0x34
               0x41});       // LD B,C

    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0x42);
    assert(m.cpu.step() == 8);
    assert(m.cpu.b() == 0x12);
    assert(m.cpu.step() == 8);
    assert(m.cpu.c() == 0x34);

    // Register-to-register is the cheap form: no immediate to fetch.
    assert(m.cpu.step() == 4);
    assert(m.cpu.b() == 0x34);
    assert(m.cpu.pc() == 0x0107);
}

void ld_r16_imm_test() {
    Machine m({0x01, 0x34, 0x12,   // LD BC,0x1234
               0x11, 0x78, 0x56,   // LD DE,0x5678
               0x21, 0xBC, 0x9A,   // LD HL,0x9ABC
               0x31, 0xFE, 0xFF}); // LD SP,0xFFFE

    assert(m.cpu.step() == 12);
    assert(m.cpu.bc() == 0x1234);
    assert(m.cpu.step() == 12);
    assert(m.cpu.de() == 0x5678);
    assert(m.cpu.step() == 12);
    assert(m.cpu.hl() == 0x9ABC);
    assert(m.cpu.step() == 12);
    assert(m.cpu.sp() == 0xFFFE);

    // The immediate is little-endian: low byte first. 0x34 0x12 -> 0x1234, not 0x3412.
    assert(m.cpu.pc() == 0x010C);
}

void ld_indirect_hl_test() {
    Machine m({0x21, 0x00, 0xC0,   // LD HL,0xC000
               0x36, 0x99,        // LD (HL),0x99
               0x7E,              // LD A,(HL)
               0x46,              // LD B,(HL)
               0x70});            // LD (HL),B

    assert(m.cpu.step() == 12); // LD HL,0xC000
    assert(m.cpu.hl() == 0xC000);

    assert(m.cpu.step() == 12); // LD (HL),0x99 -- a memory write, so 12 not 8
    assert(m.bus.read8(0xC000) == 0x99);

    assert(m.cpu.step() == 8); // LD A,(HL) -- one memory read
    assert(m.cpu.a() == 0x99);

    assert(m.cpu.step() == 8); // LD B,(HL)
    assert(m.cpu.b() == 0x99);

    m.cpu.set_b(0x11);
    assert(m.cpu.step() == 8); // LD (HL),B
    assert(m.bus.read8(0xC000) == 0x11);
}

void ld_indirect_bc_de_test() {
    Machine m({0x02,   // LD (BC),A
               0x0A,   // LD A,(BC)
               0x12,   // LD (DE),A
               0x1A}); // LD A,(DE)

    m.cpu.set_bc(0xC000);
    m.cpu.set_a(0x5A);
    assert(m.cpu.step() == 8);
    assert(m.bus.read8(0xC000) == 0x5A);

    m.cpu.set_a(0x00);
    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0x5A); // read it back through the same address

    m.cpu.set_de(0xC100);
    assert(m.cpu.step() == 8);
    assert(m.bus.read8(0xC100) == 0x5A);

    m.cpu.set_a(0x00);
    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0x5A);
}

void ld_hl_inc_dec_test() {
    // LD (HL+),A ; LD A,(HL+) ; LD (HL-),A ; LD A,(HL-) ; and one more LD (HL+),A for the
    // wrap check at the end (running off the end of this program would execute a zero byte,
    // which is a NOP, not a fifth LD).
    Machine m({0x22, 0x2A, 0x32, 0x3A, 0x22});

    m.cpu.set_hl(0xC000);
    m.cpu.set_a(0x77);

    assert(m.cpu.step() == 8);
    assert(m.bus.read8(0xC000) == 0x77);
    assert(m.cpu.hl() == 0xC001); // accessed at 0xC000, then incremented

    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0x00); // 0xC001 was never written
    assert(m.cpu.hl() == 0xC002);

    m.cpu.set_a(0x33);
    assert(m.cpu.step() == 8);
    assert(m.bus.read8(0xC002) == 0x33);
    assert(m.cpu.hl() == 0xC001); // decrement after the write

    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0x00); // 0xC001 is still zero
    assert(m.cpu.hl() == 0xC000);

    // HL is 16-bit and wraps: 0xFFFF incremented is 0x0000, not 0x10000.
    m.cpu.set_hl(0xFFFF);
    assert(m.cpu.step() == 8); // 0x22: write to 0xFFFF (no RAM there, ignored) then wrap
    assert(m.cpu.hl() == 0x0000);
}

void ld_a16_test() {
    Machine m({0xEA, 0x00, 0xC0,   // LD (0xC000),A
               0xFA, 0x00, 0xC0}); // LD A,(0xC000)

    m.cpu.set_a(0x3C);
    assert(m.cpu.step() == 16); // 4 fetch + 8 address + 4 write
    assert(m.bus.read8(0xC000) == 0x3C);

    m.cpu.set_a(0x00);
    assert(m.cpu.step() == 16);
    assert(m.cpu.a() == 0x3C);
}

void ld_sp_tests() {
    Machine m({0xF9,             // LD SP,HL
               0x08, 0x00, 0xC1, // LD (0xC100),SP
               0xF8, 0x08});     // LD HL,SP+0x08

    m.cpu.set_hl(0xFFF0);
    assert(m.cpu.step() == 8);
    assert(m.cpu.sp() == 0xFFF0);

    assert(m.cpu.step() == 20); // the most expensive LD: two bytes out to memory
    assert(m.bus.read8(0xC100) == 0xF0); // little-endian: low byte at the address
    assert(m.bus.read8(0xC101) == 0xFF);

    m.cpu.set_sp(0xFFF8);
    assert(m.cpu.step() == 12);
    assert(m.cpu.hl() == 0x0000); // 0xFFF8 + 8, wrapping past the top of memory
    assert(!m.cpu.z_flag());      // Z and N are always cleared
    assert(!m.cpu.n_flag());
    assert(m.cpu.h_flag());       // 0x8 + 0x8 carries out of bit 3
    assert(m.cpu.c_flag());       // 0xF8 + 0x08 carries out of bit 7

    // A negative offset is added, not subtracted: H and C come from the unsigned low bytes.
    Machine negative({0xF8, 0xFE});
    negative.cpu.set_sp(0xFFF8);
    assert(negative.cpu.step() == 12);
    assert(negative.cpu.hl() == 0xFFF6); // 0xFFF8 - 2
    assert(negative.cpu.h_flag());
    assert(negative.cpu.c_flag());
    assert(!negative.cpu.z_flag() && !negative.cpu.n_flag());
}

void ld_c_indirect_test() {
    Machine m({0xE2,   // LD (C),A   -> address 0xFF00 + C
               0xF2}); // LD A,(C)

    m.cpu.set_c(0x80); // 0xFF80 = start of high RAM, still unmapped today
    m.cpu.set_a(0x12);
    assert(m.cpu.step() == 8); // the write is ignored, but the addressing must not crash

    m.cpu.set_a(0x00);
    assert(m.cpu.step() == 8);
    assert(m.cpu.a() == 0xFF); // unmapped I/O reads as an open bus
}

void unimplemented_opcode_test() {
    // 0xC3 is JP nn: a real instruction, but not one this CPU knows yet.
    Machine m({0xC3, 0x37, 0x06});
    const uint16_t pc_before = m.cpu.pc();

    bool threw = false;
    try {
        m.cpu.step();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
    assert(m.cpu.pc() == pc_before); // PC is put back: the instruction did not happen

    // 0x76 looks like LD (HL),(HL) but is HALT, so it must not fall into the LD block.
    Machine halt_rom({0x76});
    bool halt_threw = false;
    try {
        halt_rom.cpu.step();
    } catch (const std::runtime_error&) {
        halt_threw = true;
    }
    assert(halt_threw);
}

} // namespace

int main() {
    // Day 3: the register file.
    pair_accessors_test();
    af_masking_test();
    flags_test();
    reset_test();

    // Day 4: fetch, decode, execute, and the LD family.
    step_without_bus_test();
    nop_test();
    ld_r8_imm_and_reg_test();
    ld_r16_imm_test();
    ld_indirect_hl_test();
    ld_indirect_bc_de_test();
    ld_hl_inc_dec_test();
    ld_a16_test();
    ld_sp_tests();
    ld_c_indirect_test();
    unimplemented_opcode_test();

    std::printf("cpu_tests: all asserts passed\n");
    return 0;
}
