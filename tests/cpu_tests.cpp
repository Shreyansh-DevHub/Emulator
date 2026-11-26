// Register-file tests for Cpu, using plain <cassert>: a failed check aborts the program,
// which is all a test needs to do here. No framework, no dependency.
//
// Build: cmake --build --preset default
// Run:   ./build/cpu_tests.exe      (or: ctest --test-dir build --output-on-failure)

// assert() disappears when NDEBUG is defined (Release builds). Undefine it before <cassert>
// so the checks still run if this is ever built optimised.
#undef NDEBUG
#include <cassert>
#include <cstdio>

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

} // namespace

int main() {
    pair_accessors_test();
    af_masking_test();
    flags_test();
    reset_test();

    std::printf("cpu_tests: all asserts passed\n");
    return 0;
}
