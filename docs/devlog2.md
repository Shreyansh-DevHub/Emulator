# Devlog 2 — What the code does today, and why it looks like this

> Companion to `00-overview.md`. This entry explains the **current** source tree line by
> line: what each piece of code is for, what the recent changes were meant to achieve, and
> what is still missing. Written for someone (including future me) opening the repo cold.

Current state: the emulator does not emulate anything yet. It **loads a real Game Boy ROM
file from disk, parses its 0x0100-0x014F header, and routes 16-bit bus reads/writes into
ROM or Work RAM**. That is the whole program. Everything below explains that.

---

## 1. Repository layout

```
Emulator/
├── CMakeLists.txt        build definition (target `gbemu`)
├── CMakePresets.json     "how to configure" — vcpkg toolchain + Ninja + Debug
├── .vscode/tasks.json    Ctrl+Shift+B build tasks (configure / build)
├── .vscode/settings.json "cmake.useCMakePresets": "always"
├── .gitignore            build output, binaries, CMakeUserPresets.json
├── src/
│   ├── main.cpp          entry point: load ROM → check result → attach to bus → print
│   └── bus.hpp           RomHeader, Rom, Bus  (header-only for now)
├── roms/                 test ROMs, e.g. cpu_instrs.gb (Blargg cpu_instrs)
├── docs/                 00-overview.md, this file
├── root/                 empty, unused
└── build/                CMake binary dir (generated, git-ignored)
```

`docs/00-overview.md` plans `src/` to eventually split into `cpu/`, `bus/`, `cartridge/`,
`ppu/`, `apu/`, `timer/`, `joypad/`, `frontend/`. Right now it is two files: the cartridge
+ bus logic lives in `bus.hpp` and the program entry lives in `main.cpp`. Splitting it is
a later refactor, not a correctness issue while there is only one `main`.

---

## 2. Build system, and the problems it was fixed to solve

### `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(gbemu CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(SDL2 CONFIG REQUIRED)
add_executable(gbemu src/main.cpp)
target_link_libraries(gbemu PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
```

- `project(gbemu CXX)` — C++ only, no C.
- C++17 is required because the code uses `std::optional` and `std::string`.
- `find_package(SDL2 CONFIG REQUIRED)` asks for SDL2's **CMake package config files**
  (`SDL2Config.cmake`), *not* a hand-rolled `FindSDL2.cmake`. vcpkg ships those config
  files, which is why vcpkg has to be the source of SDL2.
- The two `$<...>` generator expressions make the link line tolerant: `SDL2main` exists
  only when SDL2 was built with its "main" shim, and vcpkg may expose either `SDL2::SDL2`
  (shared) or `SDL2::SDL2-static`. Instead of hardcoding one, take whichever is present.
- SDL2 is linked but **not currently used**: `main.cpp` opens no window. The dependency is
  kept because the frontend returns; removing it would just have to be re-added.

### `CMakePresets.json` — the actual bug fix

The project was not buildable from a terminal. SDL2 lives in real vcpkg
(`C:\vcpkg`, triplet `x64-windows`), but nothing in the project told CMake to look there,
so `find_package(SDL2 CONFIG REQUIRED)` failed. The only working invocation was the long
manual one with `-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`, which
is easy to forget and impossible to encode in a VS Code task before a *cache* exists.

The preset stores that knowledge once:

```json
{
  "name": "default",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  }
}
```

- `$env{VCPKG_ROOT}` = `C:\vcpkg` here, so the file contains no absolute machine path and
  stays valid for anyone else with a normal vcpkg checkout.
- `Ninja` + a single-config generator keeps the build fast and simple; `Debug` because we
  want symbols while bringing up an emulator (a wrong-cycle bug is much easier to chase
  with a debugger attached than with `-O3` and inlined frames).
- `binaryDir` = `build/`, matching the git-ignored folder.

Why a preset instead of a `CMakeUserPresets.json` or docs-only instructions? Because
VS Code was already configured with `"cmake.useCMakePresets": "always"` and no presets
file existed — the editor was set up to use a mechanism the project didn't have. Adding
the presets file makes the existing editor setting correct rather than fighting it.

### `.vscode/tasks.json`

The old build task called `cl.exe`, which is **not installed / not on PATH** on this
machine, so the task could never have worked. It was replaced with two tasks that shell
out to the preset:

```jsonc
{ "label": "cmake: configure", "command": "cmake --preset default" }
{ "label": "cmake: build",     "command": "cmake --build --preset default" } // default-build
```

`cmake: build` is marked as the default build task (Ctrl+Shift+B), and uses the `$gcc`
problem matcher so compiler errors become clickable.

### `.gitignore`

`build/`, `out/`, `*.obj`, `*.exe`, `*.pdb`, `*.ilk`, `*.dll`, `CMakeUserPresets.json`,
`CMakeCache.txt`, `CMakeFiles/`. The point is that `build/` contains hundreds of generated
files plus a **copied `SDL2d.dll`**; none of it is source, all of it is regenerable.

### Toolchain caveat (still open)

`build/CMakeCache.txt` currently pins MSVC `cl.exe`
(`C:/Program Files/Microsoft Visual Studio/18/Community/...`) while `cl` is not on `PATH`
in a plain shell and `INCLUDE` is empty there, so `cl` cannot find `cstdio`:

```
fatal error C1083: Cannot open include file: 'cstdio': No such file or directory
```

Two ways to work around it: build from a **VS Developer shell** / the VS Code CMake Tools
button (which inject the MSVC environment), or delete `build/` so the preset reconfigures
with MinGW g++ from `C:\msys64\ucrt64` / WinLibs, which builds and links the vcpkg SDL2
fine. Everything described in this devlog was verified with g++.

---

## 3. `src/bus.hpp` — cartridge + memory bus

Header-only for now: one `#include "bus.hpp"` and the cartridge + bus types are available.
It begins with `#pragma once`, so a second translation unit can include it without
duplicate definitions. Printing is **not** here: `print_rom_header` lives in `main.cpp`,
next to the only code that calls it.

### 3.1 `RomHeader` — the parsed, human-readable header

```cpp
struct RomHeader {
    static constexpr size_t ENTRY_POINT_ADDR   = 0x0100; // 00 C3 xx xx (NOP; JP nn)
    static constexpr size_t TITLE_ADDR         = 0x0134; // 16 ASCII bytes...
    static constexpr size_t TITLE_MAX_LEN      = 0x10;   // ...0x0134-0x0143
    static constexpr size_t CARTRIDGE_TYPE_ADDR= 0x0147; // MBC / feature byte
    static constexpr size_t ROM_SIZE_CODE_ADDR = 0x0148; // declared ROM size code
    static constexpr size_t RAM_SIZE_CODE_ADDR = 0x0149; // declared cart RAM size
    static constexpr size_t CHECKSUM_START     = 0x0134; // checksum covers...
    static constexpr size_t CHECKSUM_END       = 0x014C; // ...0x0134-0x014C
    static constexpr size_t CHECKSUM_ADDR      = 0x014D; // the stored byte

    std::string title;
    uint32_t entry_point = 0;       // raw bytes at 0x0100-0x0103 (instructions)
    size_t   rom_size    = 0;       // actual length of the loaded file
    uint8_t  cartridge_type = 0;    // raw code at 0x0147
    uint8_t  rom_size_code = 0;     // raw code at 0x0148
    size_t   declared_rom_size = 0; // decoded; 0 = unrecognised
    uint8_t  ram_size_code = 0;     // raw code at 0x0149
    size_t   declared_ram_size = 0; // decoded; 0 = none/unrecognised
    uint8_t  checksum    = 0;       // byte stored at 0x014D
    uint8_t  computed_checksum = 0; // recomputed from 0x0134-0x014C
    bool     checksum_ok = false;   // computed == stored
};
```

The `..._ADDR` constants (and the `CHECKSUM_START`/`CHECKSUM_END` range) live next to the
decoded fields they describe, which is the only place header offsets appear in the code base. There are no window-relative magic
numbers like `0x34` or `0x4D` left anywhere.

This is a *decoded view* of the facts clipped out of the raw cartridge header, kept apart
from the raw bytes so `main` (and later the debugger) never has to know header offsets.
In the Game Boy cartridge header (see Pan Docs `0x0100-0x014F`):

| Field        | Absolute address  | Meaning here                                            |
|--------------|-------------------|---------------------------------------------------------|
| entry bytes  | `0x0100`-`0x0103` | first four ROM bytes: instructions, printed as `00 C3 37 06` |
| title        | `0x0134`-`0x0143` | up to 16 ASCII chars, NUL-terminated in practice         |
| cart type    | `0x0147`          | which MBC (if any) the cart uses, decoded to a name      |
| size code    | `0x0148`          | declared ROM size code, decoded to bytes and compared    |
| RAM code     | `0x0149`          | declared cart RAM size code, decoded to bytes            |
| checksum     | `0x014D`          | stored header checksum byte; recomputed from `0x0134`-`0x014C` and compared |
| file size    | —                 | length of the loaded file (`hdr.rom_size`)               |

### 3.2 `Rom` — an owned, read-only cartridge dump

Offsets come from `RomHeader`'s constants — `ENTRY_POINT_ADDR`, `TITLE_ADDR`,
`TITLE_MAX_LEN`, `CARTRIDGE_TYPE_ADDR`, `ROM_SIZE_CODE_ADDR`, `RAM_SIZE_CODE_ADDR`,
`CHECKSUM_START`/`CHECKSUM_END`, `CHECKSUM_ADDR` — so `Rom` itself contains no raw header
addresses and no second, relative coordinate system to keep in sync with the absolute one.

**Constructor**

```cpp
explicit Rom(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.empty()) throw std::runtime_error("ROM data cannot be empty.");
    header_ = parse_header();
}
```

- `explicit` — you cannot accidentally pass a `std::vector` where a `Rom` is expected.
- `std::move` — the byte buffer is transferred, not copied; a 1 MB ROM is not duplicated.
- The empty-data check is a **programming-error guard**, not a file-error path; file
  errors are reported through `from_file`'s `std::optional` (below). It throws because an
  empty `Rom` can never be valid and silently producing one would hide the bug.
- The header is parsed once, eagerly, in the constructor, so `header()` is a cheap getter
  and cannot fail later.

**`from_file` — the error-reporting entry point**

```cpp
static std::optional<Rom> from_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;                       // cannot open
    const std::streamsize size = file.tellg();
    if (size <= 0) return std::nullopt;                   // empty / unseekable
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) return std::nullopt;
    return Rom(std::move(buffer));                        // success
}
```

Three details that matter:

- `std::ios::binary` is mandatory on Windows: without it, `0x1A` is treated as end-of-file
  and `0x0A`/`0x0D` can be translated. A ROM is arbitrary bytes, not text.
- `std::ios::ate` opens *at the end* so `tellg()` immediately yields the file size; we then
  `seekg(0)` and read the whole thing in one `read`. One pass, no `while(!eof())` loop.
- **Return type is `std::optional<Rom>`.** This is the modern C++ answer to "may fail, but
  the failure is not exceptional": no sentinel `Rom` object, no out-parameter, no exception
  for a user typo in a path. `std::nullopt` means "nothing to give you"; a value means
  "here is a fully constructed ROM". Every failure mode of the file I/O collapses to the
  same empty optional, which the caller must check with `has_value()` before dereferencing
  with `operator*`/`operator->`.

Note the subtlety: the constructor *can* still throw (`std::bad_alloc`, or the empty-data
guard if a zero-byte file somehow got past the size check). `std::optional` reports the
*file* problem; it does not make construction infallible.

**`read` — bus-facing accessor**

```cpp
uint8_t read(size_t offset) const {
    if (offset < data_.size()) return data_[offset];
    return 0xFF;  // open-bus / out-of-bounds default
}
```

Real cartridge hardware returns floating-bus values for unmapped reads; `0xFF` is the
conventional stand-in and is what most test ROMs tolerate. Returning `0xFF` instead of
UB/`operator[]` out of range means a bad address is a wrong value, not a crash — much
easier to debug later.

**`parse_header` — how the header is decoded**

- No temporary window buffer: every field is read straight out of the cartridge through
  `read(addr)`, which returns `0xFF` for addresses past the end of the file. One accessor,
  one set of absolute addresses, nothing to copy and nothing to index-translate.
- Title: `TITLE_ADDR` (= `0x0134`) for up to `TITLE_MAX_LEN` (16) bytes. The loop stops at
  the first `0x00` terminator *and* at the real end of the file, so a truncated dump cannot
  read past the vector. Trailing spaces/tabs are trimmed because many dumps pad the title,
  and a blank title becomes `"UNTITLED"` so the printout never looks like a bug.
- `entry_point` is stored as the little-endian 32-bit value of the bytes at
  `ENTRY_POINT_ADDR` (`0x0100..0x0103`), but those bytes are the **first instructions**
  (`00 C3 xx xx` = `NOP; JP nn`), not a number. Printing them as one hex word produced the
  misleading `0x0637C300`; `print_rom_header` therefore renders them as four separate bytes
  (`00 C3 37 06`), which is what the cartridge actually contains.
- `checksum` is the byte at `CHECKSUM_ADDR` (`0x014D`), the **header checksum**
  (`x = 0; for i in 0x0134..0x014C: x = x - byte - 1`), *not* the global checksum at
  `0x014E`. It is now actually **verified**: `parse_header` runs exactly that loop over
  `CHECKSUM_START..CHECKSUM_END`, stores the result in `computed_checksum`, and sets
  `checksum_ok = (computed == stored)`. Because the loop is plain unsigned `uint8_t`
  arithmetic, wraparound modulo 256 happens for free — no explicit `& 0xFF` needed. This is
  the cheapest possible corrupt-dump detector: a single flipped byte anywhere in the title,
  the MBC byte, the size codes, or the licencee/new-licencee fields changes the sum.
- `cartridge_type` is the raw byte at `0x0147`, mapped to a mnemonic by
  `Rom::cartridge_type_name` (`0x00` → `ROM ONLY`, `0x01` → `MBC1`, `0x19` → `MBC5`, …,
  unrecognised codes → `UNKNOWN`). We only *report* it today; nothing switches banks yet.
  This is the field that decides which mapper class has to be implemented first.
- `ram_size_code` (`0x0149`) is decoded by `Rom::decode_ram_size_code` into
  `declared_ram_size`: `0x00` none, `0x01` 2 KiB, `0x02` 8 KiB, `0x03` 32 KiB, `0x04`
  128 KiB, `0x05` 64 KiB. It is printed but not yet used — cart RAM at `0xA000-0xBFFF` is
  not mapped in `Bus` at all.
- `rom_size` is the in-memory size (`data_.size()`), i.e. the actual file length. The
  header's own size code at `0x0148` is the *declared* size, kept as `rom_size_code` and
  decoded by `decode_rom_size_code` into `declared_rom_size`: `0x00-0x08` mean
  `32 KiB << code`, `0x52`/`0x53`/`0x54` are the odd 1.1/1.2/1.5 MiB carts, anything else
  decodes to `0` (unrecognised). `print_rom_header` compares the decoded size against
  `rom.size()` and prints `[matches file]` or `[MISMATCH: file holds N bytes]`, so a
  truncated or over-padded dump is visible immediately.

### 3.3 `Bus` — the memory map the CPU will talk to

```cpp
class Bus {
public:
    static constexpr uint16_t ROM_END    = 0x7FFF;
    static constexpr uint16_t WRAM_START = 0xC000;
    static constexpr uint16_t WRAM_END   = 0xDFFF;
    static constexpr size_t   WRAM_SIZE  = 0x2000;

    Bus() : wram_(WRAM_SIZE, 0x00) {}
    void attach_rom(const Rom* rom) { rom_ = rom; }

    uint8_t read8(uint16_t addr) const {
        if (addr <= ROM_END) return rom_ != nullptr ? rom_->read(addr) : 0xFF;
        if (in_wram(addr)) return wram_[addr - WRAM_START];
        return 0xFF;                     // VRAM/OAM/I-O/HRAM/IE: not mapped yet
    }

    void write8(uint16_t addr, uint8_t value) {
        if (in_wram(addr)) wram_[addr - WRAM_START] = value;
        // ROM is read-only; other regions ignore writes until they exist.
    }

private:
    static bool in_wram(uint16_t addr) { return addr >= WRAM_START && addr <= WRAM_END; }

    const Rom* rom_ = nullptr;
    std::vector<uint8_t> wram_;
};
```

This is the single choke point from `00-overview.md` §4: "the CPU never talks to
components directly, it only reads and writes addresses." Today it owns two regions; the
rest of the map (`0x8000-0x9FFF` VRAM, `0xA000-0xBFFF` cart RAM, echo RAM, OAM,
`0xFF00-0xFF7F` I/O, HRAM, `0xFFFF` IE) still falls through to `0xFF`.

- `addr` is `uint16_t` — literally a 16-bit address bus, so the compiler enforces the
  `0x0000-0xFFFF` range at the call site.
- `rom_` is a **non-owning `const Rom*`**, deliberately: the cartridge is loaded once, is
  immutable, and outlives the bus. `attach_rom` exists so a `Bus` can be constructed before
  (or without) a ROM, and so the test path can swap cartridges without rebuilding the bus.
  The cost is a lifetime rule — *the `Rom` must outlive the `Bus`* — which `main` satisfies
  by keeping the optional alive for the whole program. If that ever becomes fragile, the
  fix is `std::shared_ptr<const Rom>` (or making `Bus` own a `std::optional<Rom>`).
- The region bounds exist exactly once, as constants plus the `in_wram` helper, and both
  `read8` and `write8` ask that helper — so the map is defined in one place instead of the
  same two comparisons and `- 0xC000` arithmetic being written out twice.
- `WRAM_SIZE` (8 KB) is a named constant; WRAM is zero-filled at construction because that
  is the DMG power-on pattern.
- There is one way to give the bus a cartridge: `attach_rom`. (Previously the constructor
  also took a `const Rom*` with a `nullptr` default, i.e. a second, redundant route with a
  default argument nobody used.)
- `read8` is `const` (reading does not mutate), `write8` is not. Writes to ROM addresses are
  **ignored**, which is correct for a plain 32 KB cart and is exactly where MBC1/3/5 bank
  registers will later hook in (`0x0000-0x1FFF` enable RAM, `0x2000-0x3FFF` select bank, …).
- Known gap, important for `cpu_instrs.gb`: the Blargg ROM is **64 KB**, so `0x4000-0x7FFF`
  must be a *switchable* bank. Right now `rom_->read(addr)` indexes the same flat array, so
  the upper half always reads bank 1 of the file. Correct only until a ROM actually swaps
  banks; on a 64 KB test ROM that means the wrong code at `0x4000+` as soon as execution
  goes there. Bank switching (MBC1) is the next real cartridge task.

### 3.4 `print_rom_header` — moved to `main.cpp`

It used to live in `bus.hpp`, which forced `<iostream>` and `<iomanip>` on every file that
only wanted the cartridge and bus types. It now sits in `main.cpp`, **above** `main`
(defined before use, no forward declaration needed) and includes `<iostream>` and
`<iomanip>` itself. `bus.hpp` keeps only the data model.

It prints: title, the four entry bytes, the file size in decimal **and** hex, the cartridge
type code with its mnemonic (`0x01  MBC1`), the ROM size code with its decoded byte count and
match verdict, the RAM size code with its decoded byte count, and the header checksum as
`stored 0xNN, computed 0xNN  -> PASS|FAIL`. It resets
the stream flags with `std::dec` at the end — without that, every later
`std::cout << number` in the program would silently print in hex, a classic `std::hex`
stickiness bug.

---

## 4. `src/main.cpp` — what the program actually does now

```cpp
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
```

1. **Pick a path.** `argv[1]` if given, otherwise `roms/cpu_instrs.gb` (relative to the
   working directory, so run it from the repo root, e.g. `./build/gbemu.exe`). Accepting an
   argument keeps every other ROM in `roms/` one command away without recompiling.
2. **Load into an `std::optional<Rom>`.** The optional is the *only* indication of failure;
   it is `const` because nothing after this point may modify the cartridge.
3. **Check before use.** `has_value()` guards the dereference. On failure: message on
   `stderr` (so it can be separated from normal output and is not lost in a redirect) and
   `return 1`, a nonzero exit code scripts can detect.
4. **Attach to a bus.** `&*rom` is the address of the object *inside* the optional — valid
   for as long as `rom` is alive, which is the rest of `main`. This is the non-owning
   contract described in §3.3.
5. **Prove it works.** `print_rom_header` shows the parsed metadata; `bus.read8(0x0000)`
   shows a byte that went *through the memory bus* into the cartridge, proving the attach
   succeeded rather than just that the file opened.

Observed output for `roms/cpu_instrs.gb`:

```
=== ROM Header ===
Title       : CPU_INSTRS
Entry bytes : 00 C3 37 06  (0x0100-0x0103)
File size   : 65536 bytes (0x10000)
Size code   : 0x01  -> 65536 bytes [matches file]
Cart type   : 0x01  MBC1
RAM size    : 0x00  -> none (or unrecognised code)
Checksum    : stored 0x3B, computed 0x3B  -> PASS
First byte at 0x0000: 0x3C
```

And for a bad path: `error: could not load ROM 'roms/nope.gb'` with exit code `1`.

And for a deliberately corrupted dump (one byte of the title overwritten with `0x99`):

```
Title       : C\x99U_INSTRS
...
Checksum    : stored 0x3B, computed 0xF2  -> FAIL
```

Note the exit code stays `0`: a checksum mismatch is reported, not fatal. The ROM is still
loaded and attached, because the emulator has no opinion about whether the game may run.

---

## 5. What was removed, and why

The previous version of `bus.hpp` ended with its own `main()` that built a **synthetic
32 KB ROM in memory**:

It allocated a 32 KB zero-filled byte vector, wrote a fake entry point at `0x0100`, a
hand-typed title at `0x0134` and a fake checksum at `0x014D`, then printed the parsed header
and did one WRAM write/read pair. It was scaffolding written to exercise the parser before any real ROM was available. It had
to go for three reasons:

1. **It tested the author, not the hardware.** Hand-written bytes at hand-picked offsets only
   prove the code agrees with itself. `cpu_instrs.gb` has a real header written by a real
   assembler, so the parser now earns its keep.
2. **It hid file errors.** The in-memory path never exercised `from_file`, so "cannot open
   the file", "empty file", "short read" — the failure modes that actually happen — were
   never reached. Those paths are now the *only* way in, and both outcomes were tested.
3. **Two `main`s cannot coexist.** A header with `int main()` cannot be included by the file
   that defines the real `main`; the linker would see duplicate symbols. Removing the demo
   `main` from the header is what allows `main.cpp` to `#include "bus.hpp"` at all.

The WRAM poke/write/read demo went with it; `Bus::write8`/`read8` on `0xC000` are still
there and still correct, just no longer self-tested from within the header. A `tests/`
target is the right home for that kind of check (see §7).

---

## 6. Changes made to get here (chronological)

1. **Diagnosed a mislabelled error.** The original frontend printed
   `SDL setup failed: Parameter 'renderer' is invalid` for every failure, blaming a
   component that might not be the broken one. Replaced with ordered, specific checks
   (`SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_CreateTexture`), each printing its own
   message, cleaning up what it created, and returning `1`. Correct cleanup order:
   destroy texture → renderer → window → `SDL_Quit`.
2. **Made the build reproducible.** Added `CMakePresets.json` (Ninja, Debug, vcpkg
   toolchain from `$env{VCPKG_ROOT}`), rewrote `.vscode/tasks.json` around
   `cmake --preset` (the old task invoked an uninstalled `cl.exe`), added `.gitignore`.
   Verified from a clean `build/`: configure, build, `gbemu.exe` produced, `SDL2d.dll`
   copied next to it.
3. **Deleted `src/vc140.pdb`.** A stray MSVC debug database inside `src/` — generated
   artifact, not source; it is exactly what `.gitignore` now prevents.
4. **Replaced the fake 32 KB ROM with the real cartridge** (this devlog's main change):
   removed the synthetic buffer and the header's `main()`, made `main.cpp` load
   `roms/cpu_instrs.gb` through `Rom::from_file`, check `has_value()` with a nonzero exit
   on failure, then `Bus` + `attach_rom` + `print_rom_header`.
5. **Verified** with `g++ -std=c++17 -Wall -Wextra` (no warnings): real ROM loads and prints
   its header, missing ROM prints an error and exits `1`.
6. **Finished the header read-out.** Added the cartridge type (`0x0147`) with a
   `cartridge_type_name` lookup, the cart RAM size code (`0x0149`) with
   `decode_ram_size_code`, and a real *computed* header checksum over `0x0134`-`0x014C`
   compared against the stored byte at `0x014D` (`-> PASS` / `-> FAIL`). Checked with a copy
   of `cpu_instrs.gb` whose byte at `0x0135` was overwritten: computed `0xF2` vs stored
   `0x3B`, printed as `FAIL`.

---

## 7. What is deliberately still missing

| Gap | Where | Consequence today | Fix when |
|---|---|---|---|
| No CPU | — | nothing executes; this is a loader, not an emulator | Day 3+ |
| No bank switching | `Bus::read8` | 64 KB+ ROMs read the wrong bytes at `0x4000-0x7FFF` | MBC1 task |
| No VRAM/OAM/HRAM/I-O routing | `Bus::read8/write8` | those addresses return `0xFF` and ignore writes | PPU / I/O tasks |
| Checksum failure is only printed | `main` | a corrupt dump still loads and exits `0` | if it should ever be fatal |
| Non-owning `Rom*` in `Bus` | `Bus` | `Rom` must outlive `Bus` (true in `main`) | if it ever bites |
| No `tests/` project | — | correctness is checked by eye, not asserted | when a second component lands |
| No window / input / audio | `main.cpp` | SDL2 linked but unused | frontend day |
| MSVC cache in `build/` | `build/CMakeCache.txt` | `cmake --build --preset default` fails in a plain shell | reset `build/` |

---

## 8. How to run what exists

```bash
# from the repo root
cmake --preset default            # once, or after deleting build/
cmake --build --preset default
./build/gbemu.exe                 # defaults to roms/cpu_instrs.gb
./build/gbemu.exe roms/01-special.gb
```

Expected: the header block and `First byte at 0x0000: 0xNN`, exit `0`. A wrong or missing
path prints `error: could not load ROM '…'` and exits `1`.

### Next step (suggested)

Split `bus.hpp` into `cartridge.{hpp,cpp}` (Rom + header) and `bus.{hpp,cpp}` (memory map),
then add a `Mbc1` inside `Rom` so `0x4000-0x7FFF` follows the selected
bank. That is the last thing needed before a CPU can fetch instructions from a 64 KB Blargg
ROM correctly.
