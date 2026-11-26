#pragma once

#include <cstdint>

// The SM83 register file, and nothing else yet: no fetch, no decode, no cycles.
//
// Reference: Pan Docs, "Power-Up Sequence" (post-boot table under "CPU registers")
// and "CPU Registers and Flags".
class Cpu {
public:
    // Post-boot register state of a DMG, i.e. what the boot ROM leaves behind at PC = 0x0100.
    //
    // Pan Docs gives A = 0x01, B = 0x00, C = 0x13, D = 0x00, E = 0xD8, H = 0x01, L = 0x4D,
    // SP = 0xFFFE, PC = 0x0100, and F = "Z=1 N=0 H=? C=?" where H and C are both set unless
    // the cartridge header checksum byte is 0x00 (then both are clear). That makes F = 0xB0,
    // hence AF = 0x01B0, for every normal dump.
    //
    // These values are DMG-specific: an MGB boots with A = 0xFF (AF = 0xFFB0), the SGB with
    // A = 0x01 but C = 0x14 and H/L = 0xC0/0x60, and the CGB with A = 0x11, BC = 0x0000,
    // DE = 0x0008, HL = 0x007C. Only the DMG table is used here.
    static constexpr uint16_t POST_BOOT_AF = 0x01B0;
    static constexpr uint16_t POST_BOOT_BC = 0x0013;
    static constexpr uint16_t POST_BOOT_DE = 0x00D8;
    static constexpr uint16_t POST_BOOT_HL = 0x014D;
    static constexpr uint16_t POST_BOOT_SP = 0xFFFE;
    static constexpr uint16_t POST_BOOT_PC = 0x0100;

    // Bits of F. Only the high nibble exists; bits 0-3 of F are not stored in hardware.
    static constexpr uint8_t FLAG_Z = 0x80; // zero
    static constexpr uint8_t FLAG_N = 0x40; // subtract (BCD correction)
    static constexpr uint8_t FLAG_H = 0x20; // half carry, out of bit 3
    static constexpr uint8_t FLAG_C = 0x10; // carry / borrow
    static constexpr uint8_t FLAG_MASK = 0xF0; // everything a real F can hold

    Cpu() { reset(); }

    void reset() {
        set_af(POST_BOOT_AF);
        set_bc(POST_BOOT_BC);
        set_de(POST_BOOT_DE);
        set_hl(POST_BOOT_HL);
        sp_ = POST_BOOT_SP;
        pc_ = POST_BOOT_PC;
    }

    // --- 16-bit register pairs -------------------------------------------------
    // Pairs are *computed* from the two bytes, never stored alongside them, so the
    // two views of the same register cannot drift apart.

    uint16_t af() const { return combine(a_, f_); }
    void set_af(uint16_t value) {
        a_ = high(value);
        set_f(low(value)); // the low nibble of F is dropped here, in one place
    }

    uint16_t bc() const { return combine(b_, c_); }
    void set_bc(uint16_t value) {
        b_ = high(value);
        c_ = low(value);
    }

    uint16_t de() const { return combine(d_, e_); }
    void set_de(uint16_t value) {
        d_ = high(value);
        e_ = low(value);
    }

    // Unlike AF, HL is a plain 16-bit register with nothing reserved: no masking here.
    uint16_t hl() const { return combine(h_, l_); }
    void set_hl(uint16_t value) {
        h_ = high(value);
        l_ = low(value);
    }

    // --- 8-bit registers -------------------------------------------------------

    uint8_t a() const { return a_; }
    void set_a(uint8_t value) { a_ = value; }

    uint8_t b() const { return b_; }
    void set_b(uint8_t value) { b_ = value; }

    uint8_t c() const { return c_; }
    void set_c(uint8_t value) { c_ = value; }

    uint8_t d() const { return d_; }
    void set_d(uint8_t value) { d_ = value; }

    uint8_t e() const { return e_; }
    void set_e(uint8_t value) { e_ = value; }

    uint8_t h() const { return h_; }
    void set_h(uint8_t value) { h_ = value; }

    uint8_t l() const { return l_; }
    void set_l(uint8_t value) { l_ = value; }

    // Reading F can only ever produce a high-nibble value; writing goes through the
    // same mask, so the invariant holds no matter which setter was used.
    uint8_t f() const { return f_; }
    void set_f(uint8_t value) { f_ = flags_only(value); }

    // --- flags -----------------------------------------------------------------

    bool flag(uint8_t mask) const { return (f_ & mask) != 0; }

    void set_flag(uint8_t mask, bool on) {
        f_ = flags_only(on ? static_cast<uint8_t>(f_ | mask)
                           : static_cast<uint8_t>(f_ & ~mask));
    }

    // The flag readers are named *_flag because h()/c() are already the 8-bit
    // registers: one name, one meaning.
    bool z_flag() const { return flag(FLAG_Z); }
    void set_z_flag(bool on) { set_flag(FLAG_Z, on); }

    bool n_flag() const { return flag(FLAG_N); }
    void set_n_flag(bool on) { set_flag(FLAG_N, on); }

    bool h_flag() const { return flag(FLAG_H); }
    void set_h_flag(bool on) { set_flag(FLAG_H, on); }

    bool c_flag() const { return flag(FLAG_C); }
    void set_c_flag(bool on) { set_flag(FLAG_C, on); }

    // --- stack pointer / program counter ---------------------------------------

    uint16_t sp() const { return sp_; }
    void set_sp(uint16_t value) { sp_ = value; }

    uint16_t pc() const { return pc_; }
    void set_pc(uint16_t value) { pc_ = value; }

private:
    // (hi << 8) is evaluated in int, which cannot overflow here (max 0xFF00); the one
    // cast to uint16_t at the end is where the value becomes a 16-bit register.
    static uint16_t combine(uint8_t hi, uint8_t lo) {
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
    }

    static uint8_t high(uint16_t value) { return static_cast<uint8_t>(value >> 8); }
    static uint8_t low(uint16_t value) { return static_cast<uint8_t>(value & 0xFF); }

    static uint8_t flags_only(uint8_t value) { return static_cast<uint8_t>(value & FLAG_MASK); }

    uint8_t a_ = 0x00;
    uint8_t f_ = 0x00;
    uint8_t b_ = 0x00;
    uint8_t c_ = 0x00;
    uint8_t d_ = 0x00;
    uint8_t e_ = 0x00;
    uint8_t h_ = 0x00;
    uint8_t l_ = 0x00;

    uint16_t sp_ = 0x0000;
    uint16_t pc_ = 0x0000;
};
