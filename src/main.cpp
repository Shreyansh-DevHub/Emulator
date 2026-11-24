// Day 2: load a cartridge ROM from disk, attach it to the memory bus, print its header.
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include "bus.hpp"

namespace {

constexpr const char* kDefaultRomPath = "roms/cpu_instrs.gb";

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
    std::printf("First byte at 0x0000: 0x%02X\n", bus.read8(0x0000));

    return 0;
}
