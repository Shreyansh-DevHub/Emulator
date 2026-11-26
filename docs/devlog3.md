# Devlog 3 — The CPU register file, and the post-boot state

> Companion to `devlog2.md`. That entry ends with the cartridge and bus working and no CPU.
> This one adds `src/cpu.hpp`: the SM83 register file — eight 8-bit registers, SP, PC, the
> four 16-bit pairs, the four flags, and `reset()` to the post-boot values — plus
> `tests/cpu_tests.cpp`, the project's first test program. Nothing executes instructions yet.

---

## 1. What exists now

```
src/
├── bus.hpp     RomHeader, Rom, Bus   (unchanged, still header-only)
├── cpu.hpp     Cpu                   (new, header-only)
└── main.cpp    load ROM → bus → print header  (unchanged, does not use Cpu yet)
tests/
└── cpu_tests.cpp   assert-based register tests   (new)
```

`Cpu` is a self-contained value type: no pointer to the bus, no global state, no `main`.
`main.cpp` does not include it yet, because there is nothing to run — that changes when the
first instruction is fetched and `Cpu::step()` appears.

---

## 2. The post-boot values, and where they come from

The plan skips the boot ROM entirely, so the CPU has to *start* where the boot ROM would
have left it. I checked the numbers against Pan Docs before writing them, because "my
memory says" is exactly the kind of claim that turns into a two-day debugging session.

Pan Docs → **Power-Up Sequence** → *Console state after boot ROM hand-off → CPU registers*
has one table per model. For the DMG:

| Register | DMG value | Notes |
|---|---|---|
| A | `$01` | the value the boot ROM wrote to `$FF50` to unmap itself |
| F | `Z=1 N=0 H=? C=?` | see below |
| B, C | `$00`, `$13` | → BC = `$0013` |
| D, E | `$00`, `$D8` | → DE = `$00D8` |
| H, L | `$01`, `$4D` | → HL = `$014D` |
| SP | `$FFFE` | |
| PC | `$0100` | first byte of the cartridge's entry point |

So AF = `$01B0`, BC = `$0013`, DE = `$00D8`, HL = `$014D`, SP = `$FFFE`, PC = `$0100`.

The one value that needs a footnote is F. Pan Docs records H and C as "?" with footnote 1:
*if the cartridge header checksum byte is `$00`, C and H are clear; otherwise both are set.*
`cpu_instrs.gb` has a header checksum of `$3B`, i.e. non-zero, so H = C = 1, and with Z = 1
and N = 0 that is `%1011_0000` = `$B0`. That is where `AF = $01B0` comes from — it is the
value for *any normal dump*, not a universal constant. A ROM with a zero header checksum
boots with F = `$80`.

**These are DMG numbers.** The same table shows every other model differing:

| | DMG | MGB | SGB | CGB |
|---|---|---|---|---|
| A | `$01` | `$FF` | `$01` | `$11` |
| BC | `$0013` | `$0013` | `$0014` | `$0000` |
| DE | `$00D8` | `$00D8` | `$0000` | `$0008` |
| HL | `$014D` | `$014D` | `$C060` | `$007C` |

`reset()` hardcodes the DMG column. If a `Model` enum ever arrives (CGB support), `reset()`
becomes the place that branches on it — the shape of the function does not change.

---

## 3. `src/cpu.hpp`

### 3.1 Storage: eight bytes, two words, nothing else

```cpp
uint8_t a_, f_, b_, c_, d_, e_, h_, l_;
uint16_t sp_, pc_;
```

That is the whole state of an SM83 register file (plus the interrupt-enable latch, which
belongs to interrupts, not here). Note what is *not* stored: there is no `af_`, `bc_`, `de_`
or `hl_` member. **The pairs are derived from the bytes every time they are read**:

```cpp
static uint16_t combine(uint8_t hi, uint8_t lo) {
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}
```

### 3.2 Combining and splitting

- **Combine:** shift the high byte up by 8 and OR in the low byte. `hi << 8` is evaluated in
  `int`, which is exactly why it is safe: a `uint8_t` promoted to `int` shifted left by 8
  gives at most `0xFF00`, nowhere near `INT_MAX`, so there is no signed overflow. The result
  is then cast to `uint16_t` **once, at the point where it becomes a 16-bit register** — not
  at the shift, and not in the caller.
- **Split:** `value >> 8` for the high byte (`uint16_t` promotes to `int`, `0xFFFF >> 8` is
  `0x00FF`, no sign-extension involved because the source is unsigned) and `value & 0xFF` for
  the low byte; both are narrowed back with a single `static_cast<uint8_t>` in the helpers
  `high()` and `low()`.

Both directions live in three private static helpers, so the cast policy is written once:

```cpp
static uint8_t high(uint16_t v) { return static_cast<uint8_t>(v >> 8); }
static uint8_t low(uint16_t v)  { return static_cast<uint8_t>(v & 0xFF); }
```

The byte setters (`set_b`, `set_c`, …) are trivial assignments. The pair setters are two
lines each:

```cpp
void set_bc(uint16_t value) { b_ = high(value); c_ = low(value); }
```

### 3.3 What the AF setter must do

F only has a high nibble. Bits 0-3 do not exist in the hardware register, so they must never
be *stored*: if they were, `af()` would return them, and code that does `push af` / `pop af`
or compares F against a constant would see values a real DMG cannot produce. The setter
therefore masks:

```cpp
void set_af(uint16_t value) {
    a_ = high(value);
    set_f(low(value));            // set_f applies FLAG_MASK
}
```

`set_f` is the single choke point, so `set_af`, `set_f` and the flag helpers all end up
masked by the same rule. The low nibble is silently dropped rather than rejected: hardware
ignores what is written to it, so the emulator ignores it too. (`set_af(0xFFFF)` gives
`a == 0xFF`, `f == 0xF0`, `af() == 0xFFF0`.)

### 3.4 Does the HL setter need special handling?

No. Unlike AF, HL is a plain 16-bit register — every one of its 16 bits is a real bit, there
is no reserved nibble, no read-only field, and Pan Docs lists nothing special about it. The
only "special" thing about the HL *pair* is that a lot of instructions treat it as a memory
pointer (`(HL)`), but that is about addressing at execution time, not about how the bits are
stored. `set_hl` is therefore the same two-liner as `set_bc`, with no mask.

AF is the *only* pair that needs filtering; that asymmetry is worth remembering, because the
temptation when writing these five setters is to copy one body to all of them.

### 3.5 Flags

The masks are named constants, not magic numbers scattered through the code:

```cpp
static constexpr uint8_t FLAG_Z = 0x80;  // zero
static constexpr uint8_t FLAG_N = 0x40;  // subtract (for DAA)
static constexpr uint8_t FLAG_H = 0x20;  // half carry, out of bit 3
static constexpr uint8_t FLAG_C = 0x10;  // carry / borrow
static constexpr uint8_t FLAG_MASK = 0xF0;
```

Three operations cover everything:

```cpp
bool flag(uint8_t mask) const { return (f_ & mask) != 0; }

void set_flag(uint8_t mask, bool on) {
    f_ = flags_only(on ? static_cast<uint8_t>(f_ | mask)
                       : static_cast<uint8_t>(f_ & ~mask));
}

static uint8_t flags_only(uint8_t v) { return static_cast<uint8_t>(v & FLAG_MASK); }
```

- `f_ | mask` sets; `f_ & ~mask` clears. `~mask` is computed in `int` (`~0x80` is
  `0xFFFFFF7F`), which is why it is narrowed back with `static_cast<uint8_t>` before it is
  and-ed — otherwise the whole expression stays `int` and the compiler's narrowing rules
  start to matter.
- The `!= 0` in `flag()` is deliberate: `f_ & mask` is an `int` bit pattern, and converting
  `0x80` to `bool` is fine but writing the comparison states the intent ("is this bit set")
  and stops an implicit-conversion warning on stricter compilers.
- Named wrappers (`z_flag()`, `set_z_flag()`, …) exist so instruction code can read
  `if (cpu.z_flag())` instead of `if (cpu.flag(Cpu::FLAG_Z))`. They are *named* `*_flag`
  because `h()` and `c()` are already taken by the H and C registers — one name, one meaning.
  (`c_flag()` next to `c()` is the least pretty part of this header; the alternative would be
  `f_c()` / `reg_c()`, which is worse.)

### 3.6 `reset()`

```cpp
void reset() {
    set_af(POST_BOOT_AF);   // 0x01B0
    set_bc(POST_BOOT_BC);   // 0x0013
    set_de(POST_BOOT_DE);   // 0x00D8
    set_hl(POST_BOOT_HL);   // 0x014D
    sp_ = POST_BOOT_SP;     // 0xFFFE
    pc_ = POST_BOOT_PC;     // 0x0100
}
```

It goes through the same setters as everything else, so there is no second, subtly different
way to set a register, and it is called from the constructor — a default-constructed `Cpu` is
already a post-boot CPU, never a block of zeros.

---

## 4. `tests/cpu_tests.cpp`

Plain `<cassert>`, no framework. A failing assert aborts the process, which is all the
signal a test needs; `ctest` reports a non-zero exit as a failure.

- `#undef NDEBUG` *before* `#include <cassert>` so the checks survive being compiled with
  `-DNDEBUG` (Release). Without it a Release build would silently pass every test.
- Four functions, so a failure names its area in the abort message: `pair_accessors_test`,
  `af_masking_test`, `flags_test`, `reset_test`.
- Covered: `set_bc(0x1234)` → B = `0x12`, C = `0x34`; writing C directly moves `bc()`;
  `set_af(0xFFFF)` → F = `0xF0`; setting one flag touches only that bit; clearing one flag
  leaves the other three; `reset()` matches the post-boot table byte by byte (after the
  registers were deliberately scrambled); a default-constructed `Cpu` is already reset.

### CMake

```cmake
enable_testing()
add_executable(cpu_tests tests/cpu_tests.cpp)
add_test(NAME cpu_tests COMMAND cpu_tests)
```

`gbemu` is untouched: the tests are a second executable, not a test mode bolted into the
emulator. `add_test(... COMMAND cpu_tests)` names the *target*, and CMake substitutes its
built path, so it works on Windows (`cpu_tests.exe`) and elsewhere.

Run them with `./build/cpu_tests.exe`, or `ctest --test-dir build --output-on-failure`.

---

## 5. Devlog questions

**Why does a CPU have flags at all?**
Because almost every instruction produces more than one result. `ADD A,B` produces a sum
*and* four facts about how it was produced: was the sum zero (Z), did it carry out of bit 7
(C), did it carry out of bit 3 (H), was it a subtraction (N). Those facts are what the next
instruction usually wants: `JP Z,nn` jumps if the last result was zero, `ADC`/`SBC` use C to
chain across bytes when adding 16- or 32-bit numbers on an 8-bit ALU, and `DAA` needs N and H
to fix up BCD arithmetic. Storing them in one register means a conditional instruction only
has to test a bit — it never re-derives a condition from operands, so it can never disagree
with what the ALU decided. Flags are the CPU's one-instruction-deep memory of its own
arithmetic.

**What goes wrong if each 16-bit pair is stored separately from its two 8-bit halves?**
You get two sources of truth for one register. The hardware register is 16 bits wide and the
SM83 can address either half, so `LD B,n` and `LD BC,nn` must be the same storage. If `bc_`
and `b_`/`c_` are separate members, every write has to update both — and *every* path has to:
the pair setter, both byte setters, `reset()`, the disassembler, save states, a debugger
view, later `INC BC`, `POP BC`, `EX (SP),HL`… One missed update and `b()` and `bc()` disagree
in a way that only shows up as a wrong branch hundreds of instructions later. Deriving the
pair costs two shifts and an OR per access, which is free next to a memory fetch, and makes
the desync *unrepresentable* rather than merely unlikely. (The same argument is why the
flag setters all funnel through `flags_only`: one rule, one place to break.)

**Why must the low nibble of F always read as 0?**
Because it does not exist. F is a 4-bit latch wired to bits 7-4 of the byte the CPU can
address; bits 3-0 read back as 0 no matter what was written. Code depends on this: `PUSH AF`
followed by `POP AF` must reproduce the flags exactly, some games store the flags and compare
the byte, and the documented post-boot value `AF = $01B0` only makes sense as a value whose
low nibble is zero. Emulating it faithfully costs one `& 0xF0`, and the alternative — storing
a full byte and hoping nothing looks at bits 0-3 — is a bug that only appears in a specific
game's checksum. The mask also makes `AF` a well-defined 16-bit number, which is what the
`af()` getter returns.

---

## 6. What is deliberately still missing

| Gap | Where | Consequence today |
|---|---|---|
| No instruction fetch/decode/execute | — | the register file is never touched by `main` |
| `Cpu` is not wired to `Bus` | `main.cpp` | no memory access exists yet |
| No interrupt master enable / IF / IE | `Cpu` | needed with the first `IME` instruction |
| `reset()` hardcodes DMG | `Cpu::reset` | CGB/MGB boot values would be wrong |
| Tests are not run automatically | `CMakeLists.txt` | `cmake --build` does not run `ctest`; do it by hand |

The next real step is `fetch()`: `uint8_t op = bus.read8(pc_++);` plus a table of the 256
opcodes, starting with `NOP`, `LD`, `JP` and `ADD` — at which point `Cpu` needs a `Bus&`,
and `main` can finally run `roms/cpu_instrs.gb`'s first instructions instead of printing a
header.

---

## 7. How to build and run

```bash
cmake --preset default
cmake --build --preset default

./build/cpu_tests.exe                       # "cpu_tests: all asserts passed", exit 0
ctest --test-dir build --output-on-failure  # same thing through CTest, 1/1 passed

./build/gbemu.exe                           # unchanged: ROM header read-out
```

If the build suddenly fails with `fatal error C1083: Cannot open include file: 'cstdio'`,
VS Code's CMake Tools has re-pinned `build/CMakeCache.txt` to MSVC `cl.exe`; `rm -rf build`
and reconfigure (same caveat as `devlog2.md` §2).
