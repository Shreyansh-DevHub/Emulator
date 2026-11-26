#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Decoded cartridge header, with the field addresses it comes from.
// All addresses are absolute ROM addresses (Pan Docs 0x0100-0x014F).
struct RomHeader {
    static constexpr size_t ENTRY_POINT_ADDR = 0x0100;   // 00 C3 xx xx (NOP; JP nn)
    static constexpr size_t TITLE_ADDR = 0x0134;         // 16 ASCII bytes...
    static constexpr size_t TITLE_MAX_LEN = 0x10;        // ...0x0134-0x0143
    static constexpr size_t CARTRIDGE_TYPE_ADDR = 0x0147; // MBC / feature byte
    static constexpr size_t ROM_SIZE_CODE_ADDR = 0x0148; // declared ROM size code
    static constexpr size_t RAM_SIZE_CODE_ADDR = 0x0149; // declared cart RAM size code
    static constexpr size_t CHECKSUM_START = 0x0134;     // header checksum covers...
    static constexpr size_t CHECKSUM_END = 0x014C;       // ...0x0134-0x014C inclusive
    static constexpr size_t CHECKSUM_ADDR = 0x014D;      // the stored byte, not global

    std::string title;

    // Raw bytes at 0x0100-0x0103: the first instructions, not an address.
    uint32_t entry_point = 0;

    size_t rom_size = 0;          // actual length of the loaded file
    uint8_t cartridge_type = 0;   // raw code at 0x0147
    uint8_t rom_size_code = 0;    // raw code at 0x0148
    size_t declared_rom_size = 0; // decoded from rom_size_code; 0 = unrecognised
    uint8_t ram_size_code = 0;    // raw code at 0x0149
    size_t declared_ram_size = 0; // decoded from ram_size_code; 0 = none/unrecognised
    uint8_t checksum = 0;         // byte stored at 0x014D
    uint8_t computed_checksum = 0; // recomputed from 0x0134-0x014C
    bool checksum_ok = false;     // computed_checksum == checksum
};

class Rom {
public:
    explicit Rom(std::vector<uint8_t> data) : data_(std::move(data)) {
        if (data_.empty()) {
            throw std::runtime_error("ROM data cannot be empty.");
        }
        header_ = parse_header();
    }

    static std::optional<Rom> from_file(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return std::nullopt;
        }

        const std::streamsize size = file.tellg();
        if (size <= 0) {
            return std::nullopt;
        }

        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return std::nullopt;
        }

        return Rom(std::move(buffer));
    }

    uint8_t read(size_t offset) const {
        return offset < data_.size() ? data_[offset] : 0xFF; // open bus when unmapped
    }

    const RomHeader& header() const { return header_; }
    size_t size() const { return data_.size(); }

    // Human-readable mnemonic for the byte at 0x0147 (Pan Docs).
    static const char* cartridge_type_name(uint8_t code) {
        switch (code) {
            //this is the actual address taken from the pandocs
            case 0x00: return "ROM ONLY";
            case 0x01: return "MBC1";
            case 0x02: return "MBC1+RAM";
            case 0x03: return "MBC1+RAM+BATTERY";
            case 0x05: return "MBC2";
            case 0x06: return "MBC2+BATTERY";
            case 0x08: return "ROM+RAM";
            case 0x09: return "ROM+RAM+BATTERY";
            case 0x0B: return "MMM01";
            case 0x0C: return "MMM01+RAM";
            case 0x0D: return "MMM01+RAM+BATTERY";
            case 0x0F: return "MBC3+TIMER+BATTERY";
            case 0x10: return "MBC3+TIMER+RAM+BATTERY";
            case 0x11: return "MBC3";
            case 0x12: return "MBC3+RAM";
            case 0x13: return "MBC3+RAM+BATTERY";
            case 0x19: return "MBC5";
            case 0x1A: return "MBC5+RAM";
            case 0x1B: return "MBC5+RAM+BATTERY";
            case 0x1C: return "MBC5+RUMBLE";
            case 0x1D: return "MBC5+RUMBLE+RAM";
            case 0x1E: return "MBC5+RUMBLE+RAM+BATTERY";
            case 0x20: return "MBC6";
            case 0x22: return "MBC7+SENSOR+RUMBLE+RAM+BATTERY";
            case 0xFC: return "POCKET CAMERA";
            case 0xFD: return "BANDAI TAMA5";
            case 0xFE: return "HuC3";
            case 0xFF: return "HuC1+RAM+BATTERY";
            default: return "UNKNOWN";
        }
    }

    // 0x00: none, 0x01: 2 KiB, 0x02: 8 KiB, 0x03: 32 KiB, 0x04: 128 KiB, 0x05: 64 KiB.
    static size_t decode_ram_size_code(uint8_t code) {
        switch (code) {
            case 0x00: return 0;
            case 0x01: return 2 * 1024;
            case 0x02: return 8 * 1024;
            case 0x03: return 32 * 1024;
            case 0x04: return 128 * 1024;
            case 0x05: return 64 * 1024;
            default: return 0;
        }
    }

private:
    std::vector<uint8_t> data_;
    RomHeader header_;

    // 0x00-0x08: 32 KiB << code. 0x52-0x54: the odd 1.1/1.2/1.5 MiB carts.
    static size_t decode_rom_size_code(uint8_t code) {
        if (code <= 0x08) {
            return static_cast<size_t>(0x8000u) << code;
        }
        switch (code) {
            case 0x52: return 72 * 0x4000;
            case 0x53: return 80 * 0x4000;
            case 0x54: return 96 * 0x4000;
            default: return 0;
        }
    }

    RomHeader parse_header() const {
        RomHeader hdr;

        for (size_t i = 0; i < 4; ++i) {
            hdr.entry_point |= static_cast<uint32_t>(read(RomHeader::ENTRY_POINT_ADDR + i)) << (8 * i);
        }

        for (size_t i = 0; i < RomHeader::TITLE_MAX_LEN; ++i) {
            const size_t addr = RomHeader::TITLE_ADDR + i;
            if (addr >= data_.size()) {
                break; // truncated ROM: stop at the real end of the file
            }
            const char c = static_cast<char>(data_[addr]);
            if (c == '\0') {
                break;
            }
            hdr.title.push_back(c);
        }
        while (!hdr.title.empty() && (hdr.title.back() == ' ' || hdr.title.back() == '\t')) {
            hdr.title.pop_back();
        }
        if (hdr.title.empty()) {
            hdr.title = "UNTITLED";
        }

        hdr.rom_size = data_.size();
        hdr.cartridge_type = read(RomHeader::CARTRIDGE_TYPE_ADDR);
        hdr.rom_size_code = read(RomHeader::ROM_SIZE_CODE_ADDR);
        hdr.declared_rom_size = decode_rom_size_code(hdr.rom_size_code);
        hdr.ram_size_code = read(RomHeader::RAM_SIZE_CODE_ADDR);
        hdr.declared_ram_size = decode_ram_size_code(hdr.ram_size_code);
        hdr.checksum = read(RomHeader::CHECKSUM_ADDR);

        // Checksum: x = x - byte - 1 for every byte in 0x0134-0x014C, mod 256.
        uint8_t x = 0;
        for (size_t addr = RomHeader::CHECKSUM_START; addr <= RomHeader::CHECKSUM_END; ++addr) {
            x = static_cast<uint8_t>(x - read(addr) - 1);
        }
        hdr.computed_checksum = x;
        hdr.checksum_ok = (x == hdr.checksum);
        return hdr;
    }
};

class Bus {
public:
    static constexpr uint16_t ROM_END = 0x7FFF;
    static constexpr uint16_t WRAM_START = 0xC000;
    static constexpr uint16_t WRAM_END = 0xDFFF;
    static constexpr size_t WRAM_SIZE = 0x2000;

    Bus() : wram_(WRAM_SIZE, 0x00) {}

    void attach_rom(const Rom* rom) { rom_ = rom; }

    uint8_t read8(uint16_t addr) const {
        if (addr <= ROM_END) {
            return rom_ != nullptr ? rom_->read(addr) : 0xFF;
        }
        if (in_wram(addr)) {
            return wram_[addr - WRAM_START];
        }
        return 0xFF; // not mapped yet (VRAM, OAM, I/O, HRAM, IE)
    }

    void write8(uint16_t addr, uint8_t value) {
        if (in_wram(addr)) {
            wram_[addr - WRAM_START] = value;
        }
        // ROM is read-only; the remaining regions ignore writes until they exist.
    }

private:
    static bool in_wram(uint16_t addr) { return addr >= WRAM_START && addr <= WRAM_END; }

    const Rom* rom_ = nullptr;
    std::vector<uint8_t> wram_;
};
