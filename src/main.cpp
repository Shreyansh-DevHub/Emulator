// Day 4: the CPU is wired to the bus. Load the cartridge, then run the
// fetch-decode-execute loop and print a trace of the first instructions.
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include "bus.hpp"
#include "cpu.hpp"
#include "disasm.hpp"

namespace {

constexpr const char* kDefaultRomPath = "roms/cpu_instrs.gb";
constexpr int kTraceInstructions = 16;

void print_rom_header(const Rom& rom, std::ostream& out = std::cout) {
    const RomHeader& h = rom.header();

    out << "=== ROM Header ===\n";
    out << "Title       : " << h.title << "\n";

    // 0x0100-0x0103 hold the first instructions (NOP; JP nn), not a number.
    out << "Entry bytes :";
    for (size_t i = 0; i < 4; ++i) {
        out << ' ' << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << ((h.entry_point >> (8 * i)) & 0xFFu);
    }
    out << "  (0x0100-0x0103)\n";

    out << "File size   : " << std::dec << h.rom_size << " bytes (0x" << std::hex
        << std::uppercase << h.rom_size << ")\n";

    out << "Size code   : 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(h.rom_size_code);
    if (h.declared_rom_size == 0) {
        out << "  -> unrecognised size code\n";
    } else {
        out << std::dec << "  -> " << h.declared_rom_size << " bytes ";
        if (h.declared_rom_size == rom.size()) {
            out << "[matches file]\n";
        } else {
            out << "[MISMATCH: file holds " << rom.size() << " bytes]\n";
        }
    }

    out << "Cart type   : 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(h.cartridge_type) << std::dec << "  "
        << Rom::cartridge_type_name(h.cartridge_type) << "\n";

    out << "RAM size    : 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(h.ram_size_code) << std::dec;
    if (h.declared_ram_size == 0) {
        out << "  -> none (or unrecognised code)\n";
    } else {
        out << "  -> " << h.declared_ram_size << " bytes\n";
    }

    // The header checksum is x = x - byte - 1 over 0x0134-0x014C, mod 256.
    out << "Checksum    : stored 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(h.checksum) << ", computed 0x" << std::setw(2)
        << static_cast<unsigned>(h.computed_checksum) << std::dec << "  -> "
        << (h.checksum_ok ? "PASS" : "FAIL") << "\n";
}

std::string registers(const Cpu& cpu) {
    return "AF=" + hex_digits(cpu.af(), 4) + " BC=" + hex_digits(cpu.bc(), 4) +
           " DE=" + hex_digits(cpu.de(), 4) + " HL=" + hex_digits(cpu.hl(), 4) +
           " SP=" + hex_digits(cpu.sp(), 4);
}

// Runs up to `limit` instructions from wherever PC is, printing each one *before* it
// executes: PC, the bytes it came from, the mnemonic, then the register state it left.
// Stops at the first opcode the CPU does not implement yet.
void print_trace(Cpu& cpu, const Bus& bus, int limit, std::ostream& out = std::cout) {
    out << "\n=== Execution trace ===\n";
    // setfill and the alignment flags are sticky on the stream: print_rom_header leaves
    // '0' behind as the fill character, which would pad these columns with zeroes.
    out << std::setfill(' ') << "PC      bytes      instruction         registers after\n";

    int executed = 0;
    bool stopped = false;

    for (int i = 0; i < limit; ++i) {
        const uint16_t pc = cpu.pc();
        const Disassembled instruction = disassemble(bus, pc);

        std::string bytes;
        for (uint8_t k = 0; k < instruction.length; ++k) {
            if (k != 0) {
                bytes += ' ';
            }
            bytes += hex_digits(bus.read8(static_cast<uint16_t>(pc + k)), 2);
        }

        out << "0x" << hex_digits(pc, 4) << "  " << std::left << std::setw(10) << bytes
            << std::setw(20) << instruction.text << std::right;

        try {
            const int cycles = cpu.step();
            ++executed;
            out << ' ' << registers(cpu) << "  (" << cycles << " cyc)\n";
        } catch (const std::exception& error) {
            out << " stopped: " << error.what() << "\n";
            stopped = true;
            break;
        }
    }

    out << "executed " << executed << " instruction(s), PC = 0x" << hex_digits(cpu.pc(), 4);
    if (stopped) {
        out << "  (halted on an opcode this CPU does not implement yet)";
    }
    out << "\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : kDefaultRomPath;

    const std::optional<Rom> rom = Rom::from_file(path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "error: could not load ROM '%s'\n", path.c_str());
        return 1;
    }

    Bus bus;
    bus.attach_rom(&*rom);

    print_rom_header(*rom);

    Cpu cpu;
    cpu.attach_bus(&bus);
    cpu.reset();

    print_trace(cpu, bus, kTraceInstructions);

    return 0;
}
