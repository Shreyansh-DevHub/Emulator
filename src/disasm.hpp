#pragma once

#include <cstdint>
#include <string>

#include "bus.hpp"

// A tiny disassembler for exactly the instructions the CPU can execute right now: NOP
// and the LD family. It exists so the execution trace can say "LD A,0x42" instead of
// "0x3E 0x42", and it grows with the CPU — the switch here and the switch in
// Cpu::execute() are meant to cover the same opcodes.
//
// An unknown opcode comes back as "?? (not implemented)" with length 1. Guessing at
// bytes that the CPU cannot run would only hide the boundary.

struct Disassembled {
    std::string text;
    uint8_t length = 1;
};

// Fixed-width, uppercase, no prefix: hex_digits(0x42, 2) == "42".
inline std::string hex_digits(uint32_t value, int digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out(static_cast<size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

inline const char* reg8_name(uint8_t index) {
    static const char* const kNames[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    return kNames[index & 0x07u];
}

inline const char* reg16_name(uint8_t index) {
    static const char* const kNames[4] = {"BC", "DE", "HL", "SP"};
    return kNames[index & 0x03u];
}

// Little-endian immediate at addr: low byte at addr, high byte at addr+1.
inline uint16_t word_at(const Bus& bus, uint16_t addr) {
    const uint8_t lo = bus.read8(addr);
    const uint8_t hi = bus.read8(static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>(static_cast<uint16_t>(hi) << 8 | lo);
}

// 0xF8's operand is signed: 0xFE reads as "-0x02", not as 254.
inline std::string signed_hex(uint8_t value) {
    const int offset = static_cast<int>(static_cast<int8_t>(value));
    if (offset < 0) {
        return "-0x" + hex_digits(static_cast<uint32_t>(-offset), 2);
    }
    return "+0x" + hex_digits(static_cast<uint32_t>(offset), 2);
}

inline Disassembled disassemble(const Bus& bus, uint16_t pc) {
    const uint8_t op = bus.read8(pc);
    const uint8_t first = bus.read8(static_cast<uint16_t>(pc + 1));

    switch (op) {
        case 0x00: return {"NOP", 1};

        // LD r16,imm16 — the destination is bits 5-4: 0x01 BC, 0x11 DE, 0x21 HL, 0x31 SP.
        case 0x01:
        case 0x11:
        case 0x21:
        case 0x31: {
            const std::string name = reg16_name(static_cast<uint8_t>(op >> 4));
            return {"LD " + name + ",0x" + hex_digits(word_at(bus, static_cast<uint16_t>(pc + 1)), 4), 3};
        }

        // LD r8,imm8 — the destination is bits 5-3: 0x06 B ... 0x3E A.
        case 0x06:
        case 0x0E:
        case 0x16:
        case 0x1E:
        case 0x26:
        case 0x2E:
        case 0x3E: {
            const std::string name = reg8_name(static_cast<uint8_t>((op >> 3) & 0x07));
            return {"LD " + name + ",0x" + hex_digits(first, 2), 2};
        }

        case 0x36: return {"LD (HL),0x" + hex_digits(first, 2), 2};

        case 0x02: return {"LD (BC),A", 1};
        case 0x12: return {"LD (DE),A", 1};
        case 0x0A: return {"LD A,(BC)", 1};
        case 0x1A: return {"LD A,(DE)", 1};

        case 0x22: return {"LD (HL+),A", 1};
        case 0x32: return {"LD (HL-),A", 1};
        case 0x2A: return {"LD A,(HL+)", 1};
        case 0x3A: return {"LD A,(HL-)", 1};

        case 0xE2: return {"LD (C),A", 1};
        case 0xF2: return {"LD A,(C)", 1};

        case 0xEA: return {"LD (0x" + hex_digits(word_at(bus, static_cast<uint16_t>(pc + 1)), 4) + "),A", 3};
        case 0xFA: return {"LD A,(0x" + hex_digits(word_at(bus, static_cast<uint16_t>(pc + 1)), 4) + ")", 3};
        case 0x08: return {"LD (0x" + hex_digits(word_at(bus, static_cast<uint16_t>(pc + 1)), 4) + "),SP", 3};

        case 0xF9: return {"LD SP,HL", 1};
        case 0xF8: return {"LD HL,SP" + signed_hex(first), 2};

        // A real instruction, just not implemented yet: naming it is not a guess.
        case 0x76: return {"HALT", 1};

        default: break;
    }

    // LD r8,r8: dst in bits 5-3, src in bits 2-0.
    if (op >= 0x40 && op <= 0x7F) {
        const std::string dst = reg8_name(static_cast<uint8_t>((op >> 3) & 0x07));
        const std::string src = reg8_name(static_cast<uint8_t>(op & 0x07));
        return {"LD " + dst + "," + src, 1};
    }

    return {"?? (not implemented)", 1};
}
