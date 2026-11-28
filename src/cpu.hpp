#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "bus.hpp"

// The SM83: the register file plus a fetch-decode-execute loop for the instructions
// implemented so far (NOP and the LD family). Every memory access goes through the Bus;
// the CPU never touches a cartridge or a RAM array itself.
//
// Reference: Pan Docs, "Power-Up Sequence" (post-boot table under "CPU registers"),
// "CPU Registers and Flags", "CPU Instruction Set".
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

    // --- memory ---------------------------------------------------------------
    // The bus is attached, not owned — the same shape as Bus::attach_rom. The CPU knows
    // how to address memory, not where the memory lives. A Cpu with no bus is still a
    // usable register file; only step() needs one.

    void attach_bus(Bus* bus) { bus_ = bus; }

    // --- fetch / decode / execute ---------------------------------------------
    // Runs exactly one instruction and returns its cost in T-cycles (the 4.194304 MHz
    // clock the PPU and timer will be fed from). PC is left on the next instruction.

    int step() {
        if (bus_ == nullptr) {
            throw std::runtime_error("Cpu::step(): no bus attached");
        }

        const uint16_t at = pc_; // start of this instruction, for error messages
        cycles_ = 0;
        const uint8_t opcode = fetch8(); // decode begins after PC has moved on

        try {
            execute(opcode, at);
        } catch (...) {
            // A failed instruction must leave the CPU where it was: restore PC so the
            // caller can stop, log, or retry on the same instruction.
            pc_ = at;
            throw;
        }

        return cycles_;
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

    // --- bus access ------------------------------------------------------------
    // step() has already checked that a bus is attached, so these are unconditional.

    uint8_t read8(uint16_t addr) const { return bus_->read8(addr); }
    void write8(uint16_t addr, uint8_t value) { bus_->write8(addr, value); }

    // Fetching is nothing more than reading at PC and letting PC advance by one.
    uint8_t fetch8() { return read8(pc_++); }

    // Immediates are little-endian, like everything else on this machine: low byte first.
    uint16_t fetch16() {
        const uint8_t lo = fetch8();
        const uint8_t hi = fetch8();
        return combine(hi, lo);
    }

    // A 16-bit store is two byte stores; +1 wraps inside uint16_t, as it does on the bus.
    void write16(uint16_t addr, uint16_t value) {
        write8(addr, low(value));
        write8(static_cast<uint16_t>(addr + 1), high(value));
    }

    // --- the r8 field of an opcode ---------------------------------------------
    // 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A.
    //
    // Index 6 is a *memory* access, which is why these are value accessors and not a
    // `uint8_t& reg(int)` returning a reference: there is no variable at (HL) that could
    // be referenced, only an address on the bus.

    uint8_t reg(size_t index) const {
        switch (index) {
            case 0: return b_;
            case 1: return c_;
            case 2: return d_;
            case 3: return e_;
            case 4: return h_;
            case 5: return l_;
            case 6: return read8(hl());
            default: return a_;
        }
    }

    void set_reg(size_t index, uint8_t value) {
        switch (index) {
            case 0: b_ = value; break;
            case 1: c_ = value; break;
            case 2: d_ = value; break;
            case 3: e_ = value; break;
            case 4: h_ = value; break;
            case 5: l_ = value; break;
            case 6: write8(hl(), value); break;
            default: a_ = value; break;
        }
    }

    // --- decode and execute -----------------------------------------------------
    // One switch, one case per opcode. There is no 256-entry function table yet: that is
    // a refactor for when the cases stop fitting on a screen, not before.
    //
    // Cycle counts are the documented T-cycles for the whole instruction, assigned
    // outright rather than accumulated, so a forgotten assignment leaves 0 behind
    // instead of a plausible-looking wrong number.

    void execute(uint8_t opcode, uint16_t at) {
        switch (opcode) {
            // --- NOP ---------------------------------------------------------
            case 0x00: cycles_ = 4; return;

            // --- LD r16,imm16 (4 fetch + 8 immediate = 12) --------------------
            case 0x01: set_bc(fetch16()); cycles_ = 12; return;
            case 0x11: set_de(fetch16()); cycles_ = 12; return;
            case 0x21: set_hl(fetch16()); cycles_ = 12; return;
            case 0x31: set_sp(fetch16()); cycles_ = 12; return;

            // --- LD r8,imm8 (4 fetch + 4 immediate = 8) -----------------------
            case 0x06: set_b(fetch8()); cycles_ = 8; return;
            case 0x0E: set_c(fetch8()); cycles_ = 8; return;
            case 0x16: set_d(fetch8()); cycles_ = 8; return;
            case 0x1E: set_e(fetch8()); cycles_ = 8; return;
            case 0x26: set_h(fetch8()); cycles_ = 8; return;
            case 0x2E: set_l(fetch8()); cycles_ = 8; return;
            case 0x3E: set_a(fetch8()); cycles_ = 8; return;

            // LD (HL),imm8 is the odd one out: same shape, but the destination is a
            // 12-cycle memory write, so it costs 12 rather than 8.
            case 0x36: write8(hl(), fetch8()); cycles_ = 12; return;

            // --- LD (r16),A and LD A,(r16) (8 cycles each way) ----------------
            case 0x02: write8(bc(), a()); cycles_ = 8; return;
            case 0x12: write8(de(), a()); cycles_ = 8; return;
            case 0x0A: set_a(read8(bc())); cycles_ = 8; return;
            case 0x1A: set_a(read8(de())); cycles_ = 8; return;

            // --- LD (HL+/-),A and LD A,(HL+/-) --------------------------------
            // The access happens first, then HL moves, so ALWAYS read the old HL.
            case 0x22: write8(hl(), a()); set_hl(static_cast<uint16_t>(hl() + 1)); cycles_ = 8; return;
            case 0x32: write8(hl(), a()); set_hl(static_cast<uint16_t>(hl() - 1)); cycles_ = 8; return;
            case 0x2A: set_a(read8(hl())); set_hl(static_cast<uint16_t>(hl() + 1)); cycles_ = 8; return;
            case 0x3A: set_a(read8(hl())); set_hl(static_cast<uint16_t>(hl() - 1)); cycles_ = 8; return;

            // --- LD (C),A and LD A,(C) ---------------------------------------
            // "(C)" means the I/O page: the high byte is implied to be 0xFF.
            case 0xE2: write8(static_cast<uint16_t>(0xFF00 + c()), a()); cycles_ = 8; return;
            case 0xF2: set_a(read8(static_cast<uint16_t>(0xFF00 + c()))); cycles_ = 8; return;

            // --- LD (nn),A and LD A,(nn) (4 + 8 + 4 = 16) --------------------
            case 0xEA: write8(fetch16(), a()); cycles_ = 16; return;
            case 0xFA: set_a(read8(fetch16())); cycles_ = 16; return;

            // --- LD (nn),SP (4 + 8 + 8 = 20, the most expensive LD) ----------
            case 0x08: write16(fetch16(), sp()); cycles_ = 20; return;

            // --- LD SP,HL (8) ------------------------------------------------
            case 0xF9: set_sp(hl()); cycles_ = 8; return;

            // --- LD HL,SP+e8 (12) --------------------------------------------
            case 0xF8: {
                const int8_t offset = static_cast<int8_t>(fetch8());
                const uint8_t lo = low(sp());
                const uint8_t unsigned_offset = static_cast<uint8_t>(offset);

                // The H and C flags come from adding the *low bytes* as unsigned 8-bit
                // values; a negative offset is not a subtraction here. Z and N are
                // always cleared — this is an address calculation, not arithmetic.
                set_z_flag(false);
                set_n_flag(false);
                set_h_flag(static_cast<int>((lo & 0x0F) + (unsigned_offset & 0x0F)) > 0x0F);
                set_c_flag(static_cast<int>(lo) + static_cast<int>(unsigned_offset) > 0xFF);
                set_hl(static_cast<uint16_t>(static_cast<int>(sp()) + offset));
                cycles_ = 12;
                return;
            }
        }

        // --- LD r8,r8: every opcode from 0x40 to 0x7F -------------------------
        // The two 3-bit fields of the opcode *are* the operands: dst in bits 5-3, src in
        // bits 2-0. The one exception is 0x76, which is HALT, not LD (HL),(HL).
        if (opcode >= 0x40 && opcode <= 0x7F) {
            if (opcode == 0x76) {
                throw unimplemented(opcode, at);
            }
            const size_t dst = static_cast<size_t>((opcode >> 3) & 0x07);
            const size_t src = static_cast<size_t>(opcode & 0x07);
            set_reg(dst, reg(src));
            // Going through (HL) is a memory access: 4 extra cycles, either direction.
            cycles_ = (dst == 6 || src == 6) ? 8 : 4;
            return;
        }

        // Everything else belongs to a later day. Refusing loudly beats guessing: a
        // silently ignored opcode looks like a CPU bug three instructions later.
        throw unimplemented(opcode, at);
    }

    static std::string hex(uint32_t value, int digits) {
        static const char kDigits[] = "0123456789ABCDEF";
        std::string out(static_cast<size_t>(digits), '0');
        for (int i = digits - 1; i >= 0; --i) {
            out[static_cast<size_t>(i)] = kDigits[value & 0xF];
            value >>= 4;
        }
        return out;
    }

    static std::runtime_error unimplemented(uint8_t opcode, uint16_t at) {
        return std::runtime_error("unimplemented opcode 0x" + hex(opcode, 2) + " at 0x" +
                                  hex(at, 4));
    }

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

    Bus* bus_ = nullptr; // not owned: whatever holds the machine owns the bus
    int cycles_ = 0;     // T-cycles of the instruction step() is running
};
