# Devlog 4 — The CPU on the bus, and the first instructions that execute

> Companion to `devlog3.md`. That entry ended with a register file that nothing called.
> This one connects `Cpu` to the `Bus`, adds the fetch-decode-execute loop, implements
> **NOP** and the whole **LD** family, adds a small disassembler so an execution trace can be
> read, and traces the first instructions of a real cartridge. Arithmetic (`ADD`, `INC`,
> `CP`), control flow (`JP`, `JR`, `CALL`, `RET`) and the `CB` page are still missing — which
> is exactly what the traces below show.

---

## 1. What exists now

```
src/
├── bus.hpp      RomHeader, Rom, Bus      (unchanged)
├── cpu.hpp      Cpu                      (+ Bus*, step(), execute(), 40 opcodes)
├── disasm.hpp   Disassembled, disassemble()   (new: for the trace only)
└── main.cpp     load ROM → bus → cpu → trace 16 instructions
tests/
└── cpu_tests.cpp   register tests + 10 instruction tests
```

`main` no longer prints `First byte at 0x0000` — the trace replaces it, because now the
program *reads memory the way the CPU does* instead of calling `bus.read8` by hand.

---

## 2. Wiring the CPU to the bus

```cpp
void attach_bus(Bus* bus) { bus_ = bus; }
...
Bus* bus_ = nullptr;   // not owned: whatever holds the machine owns the bus
```

Same shape as `Bus::attach_rom(const Rom*)`, on purpose:

- **The CPU addresses memory; it does not own it.** If `Cpu` held a `Bus` by value, the
  cartridge and its 8 KB of WRAM would be inside the CPU, and the PPU/timer/DMA would each
  need their own view of the same memory. A pointer keeps one machine, one memory, many
  users.
- **A pointer, not `Bus&`.** A reference must be bound in the constructor and can never be
  re-bound, which would make `Cpu` impossible to default-construct — and every one of the
  day-3 register tests does exactly that. `Cpu` with no bus is still a perfectly good
  register file; only `step()` needs memory, so only `step()` checks:

```cpp
if (bus_ == nullptr) throw std::runtime_error("Cpu::step(): no bus attached");
```

  The check is once per instruction, not once per byte, so it costs nothing measurable and
  turns a null dereference into a message.

---

## 3. Fetch, decode, execute

```cpp
int step() {
    if (bus_ == nullptr) throw std::runtime_error("Cpu::step(): no bus attached");

    const uint16_t at = pc_;          // start of this instruction, for error messages
    cycles_ = 0;
    const uint8_t opcode = fetch8();  // decode begins after PC has moved on

    try {
        execute(opcode, at);
    } catch (...) {
        pc_ = at;                     // the instruction did not happen: leave PC on it
        throw;
    }
    return cycles_;
}
```

Three decisions worth naming:

1. **`step()` returns T-cycles.** Everything that will be added later — the timer's
   `DIV`/`TIMA`, the PPU's 456-cycles-per-scanline, the APU frame sequencer — is driven by
   the same clock, and they all tick *by however many cycles the last instruction took*.
   Returning the cost per instruction (rather than counting inside the CPU) is what lets
   `main` become the machine loop:
   `cycles = cpu.step(); timer.tick(cycles); ppu.tick(cycles); ...`
2. **PC advances during the fetch, not after the execute.** `fetch8()` is
   `return read8(pc_++);`. The alternative — decode from `pc_`, then add the instruction
   length at the end — means every opcode case has to know its own length twice: once to
   fetch operands and once to skip them. With `pc_++` inside the fetch, an operand read and
   "PC moved past the operand" are the same statement and cannot disagree. This is also why
   `at` is captured first: after the fetch, the address of the instruction is only in a local.
3. **A failed instruction restores PC.** An unimplemented opcode throws, and the catch puts
   `pc_` back on the instruction that failed. A trace can then stop and report *where*, and
   the machine is left in a state that can be retried or stepped over later instead of being
   one byte into nowhere.

### Cycles

Cycle counts are assigned outright per case, never accumulated:

```cpp
case 0x3E: set_a(fetch8()); cycles_ = 8; return;   // LD A,imm8
```

`cycles_ = 0` at the top of `step()` means a case that forgets its count returns **0** — a
value that screams "bug" — instead of inheriting the previous instruction's plausible-looking
number. Accumulating (`+= 4` in `fetch8`, `+= 8` for an immediate) reads nicely but makes
every total the sum of contributions from three places; the Pan Docs table is per
instruction, so the code is per instruction too, and it can be checked line by line against
the table.

---

## 4. What NOP and "the LD instructions" actually are

`LD` is the largest family in the instruction set: 44 of the 245 implemented opcodes in a
full SM83. Two of them are not obvious:

| Group | Opcodes | Cycles | Note |
|---|---|---|---|
| `NOP` | `0x00` | 4 | changes nothing at all |
| `LD r16,imm16` | `01 11 21 31` | 12 | little-endian immediate |
| `LD r8,imm8` | `06 0E 16 1E 26 2E 3E` | 8 | destination in bits 5-3 |
| `LD (HL),imm8` | `36` | 12 | same shape, memory destination |
| `LD (BC)/(DE),A`, `LD A,(BC)/(DE)` | `02 12 0A 1A` | 8 | register pair as pointer |
| `LD (HL+/-),A`, `LD A,(HL+/-)` | `22 32 2A 3A` | 8 | access first, *then* HL moves |
| `LD (C),A`, `LD A,(C)` | `E2 F2` | 8 | "(C)" means `0xFF00 + C` |
| `LD (nn),A`, `LD A,(nn)` | `EA FA` | 16 | absolute address |
| `LD (nn),SP` | `08` | 20 | two bytes out to memory |
| `LD SP,HL` | `F9` | 8 | no flags |
| `LD HL,SP+e8` | `F8` | 12 | signed offset, H/C from the low bytes |
| `LD r8,r8` | `40`-`7F` except `76` | 4 or 8 | 8 if either side is `(HL)` |

**`0x76` is not `LD (HL),(HL)`.** In the `0x40`-`0x7F` block the two 3-bit fields of the
opcode *are* the operands, so the block decodes arithmetically instead of as 63 cases:

```cpp
if (opcode >= 0x40 && opcode <= 0x7F) {
    if (opcode == 0x76) throw unimplemented(opcode, at);   // it is HALT
    const size_t dst = (opcode >> 3) & 0x07;
    const size_t src = opcode & 0x07;
    set_reg(dst, reg(src));
    cycles_ = (dst == 6 || src == 6) ? 8 : 4;
    return;
}
```

The trap is that `0x76` fits the pattern perfectly (`dst = src = 6` → `LD (HL),(HL)`), so a
mask-and-shift decoder that forgets the exception silently ships a HALT that copies memory to
itself. `0x76` is a named test case for that reason.

**`(HL)` is not a register.** That is why `reg()`/`set_reg()` return and take *values*
instead of handing out `uint8_t&`:

```cpp
uint8_t reg(size_t index) const {
    switch (index) {
        case 0: return b_; ... case 5: return l_;
        case 6: return read8(hl());   // a memory access, not a variable
        default: return a_;
    }
}
```

A reference-returning `uint8_t& reg(int)` would have to return a reference to a *temporary*
for index 6 — undefined behaviour that happens to compile. Values make the memory case
expressible, at the cost of the two switches being written out in full.

**`LD HL,SP+e8` (`0xF8`) is the fiddly one.** The operand is signed — `0xFF` means `-1` — but
the flags are *not* computed from 16-bit signed arithmetic. Pan Docs defines H and C from
adding the two **low bytes as unsigned**: `(SP & 0xFF) + (e8 & 0xFF)`.

```cpp
const int8_t  offset          = static_cast<int8_t>(fetch8());
const uint8_t lo              = low(sp());
const uint8_t unsigned_offset = static_cast<uint8_t>(offset);

set_z_flag(false);            // always clear
set_n_flag(false);            // always clear
set_h_flag(static_cast<int>((lo & 0x0F) + (unsigned_offset & 0x0F)) > 0x0F);
set_c_flag(static_cast<int>(lo) + static_cast<int>(unsigned_offset) > 0xFF);
set_hl(static_cast<uint16_t>(static_cast<int>(sp()) + offset));
```

So `SP = 0xFFF8`, `e8 = 0xFE`: the result is `0xFFF6` (a subtraction), but H and C are set as
if `0xF8 + 0xFE` carried — which it does, out of bit 3 and out of bit 7. Reading it as
"`SP - 2`, and `-2` is `0xFE`, so no carry" gets both flags wrong. `LD SP,HL` (`0xF9`), by
contrast, touches no flags at all.

**16-bit stores are two byte stores**, low byte first, and `+1` wraps inside `uint16_t`:

```cpp
void write16(uint16_t addr, uint16_t value) {
    write8(addr, low(value));
    write8(static_cast<uint16_t>(addr + 1), high(value));
}
```

This will be the same helper `PUSH`, `CALL` and interrupt dispatch need, which is why it is
here already.

---

## 5. `src/disasm.hpp` — enough disassembly to read a trace

The trace would be unreadable as bare hex, so `disassemble(bus, pc)` returns
`{text, length}` for exactly the opcodes `execute()` can run — the two switches are meant to
cover the same set. Unknown opcodes come back as `?? (not implemented)` with length 1;
decoding mnemonics for opcodes the CPU cannot run would only blur the boundary that the
trace exists to show.

`length` is what lets the trace print the instruction's own bytes (`3E 42`, `21 00 C0`) by
reading `length` bytes from `pc` *before* executing — the bytes are read through the same bus
the CPU will use, so a mismatch between the printed bytes and what executed is impossible.

---

## 6. The traces

### 6.1 A real cartridge: `roms/cpu_instrs.gb`

```
=== Execution trace ===
PC      bytes      instruction         registers after
0x0100  00        NOP                  AF=01B0 BC=0013 DE=00D8 HL=014D SP=FFFE  (4 cyc)
0x0101  C3        ?? (not implemented) stopped: unimplemented opcode 0xC3 at 0x0101
executed 1 instruction(s), PC = 0x0101  (halted on an opcode this CPU does not implement yet)
```

That single line is the whole day, honestly reported:

- The post-boot register state survived: `AF=01B0 BC=0013 DE=00D8 HL=014D SP=FFFE` is the
  Pan Docs DMG table from devlog 3, still intact after an instruction ran.
- `PC` went `0x0100 → 0x0101`, exactly 4 cycles, and `0x0100` really is `NOP` in the
  cartridge — the byte came out of the ROM file, through `Bus`, through `Cpu`.
- `0x0101` holds `0xC3`, which is `JP 0x0637`. Blargg's ROMs all start `00 C3 xx xx`
  (NOP; JP entry), so **the first instruction that is not NOP or LD is at offset 1 of every
  test ROM.** The trace stops there because `JP` is not implemented yet, and it says so
  instead of pretending.

So the day-4 deliverable is "the loop runs and reads a real ROM correctly", not "the ROM
runs". Arithmetic and control flow are the next entries.

### 6.2 A hand-made LD-only cartridge

To watch the loop actually loop, I assembled a 28-byte program by hand and padded it into a
32 KiB image (roms/ holds real cartridges, so the scratch file lives in the git-ignored
`build/`):

```bash
head -c 32768 /dev/zero > build/day4-ld-demo.gb
printf '\x3e\x42\x06\x12\x0e\x34\x41\x21\x00\xc0\x36\x99\x2a\x32\xea\x02\xc0\xfa\x02\xc0\x22\x31\xfe\xff\xf9\x08\x00\xc0\x76' \
  | dd of=build/day4-ld-demo.gb bs=1 seek=256 conv=notrunc status=none
printf '\xe7' | dd of=build/day4-ld-demo.gb bs=1 seek=333 conv=notrunc status=none   # 0x014D: valid header
./build/gbemu.exe build/day4-ld-demo.gb
```

The program and its trace:

| Bytes | Instruction | What it proves |
|---|---|---|
| `3E 42` | `LD A,0x42` | immediate load, 8 cycles |
| `06 12`, `0E 34`, `41` | `LD B,0x12`, `LD C,0x34`, `LD B,C` | the 4-cycle register-to-register form |
| `21 00 C0` | `LD HL,0xC000` | 16-bit immediate, little-endian |
| `36 99` | `LD (HL),0x99` | write to WRAM through the bus |
| `2A`, `32` | `LD A,(HL+)`, `LD (HL-),A` | access-then-move ordering of HL |
| `EA 02 C0`, `FA 02 C0` | `LD (0xC002),A`, `LD A,(0xC002)` | absolute addressing, 16 cycles |
| `22` | `LD (HL+),A` | write-back into WRAM |
| `31 FE FF`, `F9` | `LD SP,0xFFFE`, `LD SP,HL` | SP loads |
| `08 00 C0` | `LD (0xC000),SP` | the 20-cycle two-byte store |
| `76` | `HALT` | stops the trace (not implemented) |

```
0x0100  3E 42     LD A,0x42            AF=42B0 BC=0013 DE=00D8 HL=014D SP=FFFE  (8 cyc)
0x0102  06 12     LD B,0x12            AF=42B0 BC=1213 DE=00D8 HL=014D SP=FFFE  (8 cyc)
0x0104  0E 34     LD C,0x34            AF=42B0 BC=1234 DE=00D8 HL=014D SP=FFFE  (8 cyc)
0x0106  41        LD B,C               AF=42B0 BC=3434 DE=00D8 HL=014D SP=FFFE  (4 cyc)
0x0107  21 00 C0  LD HL,0xC000         AF=42B0 BC=3434 DE=00D8 HL=C000 SP=FFFE  (12 cyc)
0x010A  36 99     LD (HL),0x99         AF=42B0 BC=3434 DE=00D8 HL=C000 SP=FFFE  (12 cyc)
0x010C  2A        LD A,(HL+)           AF=99B0 BC=3434 DE=00D8 HL=C001 SP=FFFE  (8 cyc)
0x010D  32        LD (HL-),A           AF=99B0 BC=3434 DE=00D8 HL=C000 SP=FFFE  (8 cyc)
0x010E  EA 02 C0  LD (0xC002),A        AF=99B0 BC=3434 DE=00D8 HL=C000 SP=FFFE  (16 cyc)
0x0111  FA 02 C0  LD A,(0xC002)        AF=99B0 BC=3434 DE=00D8 HL=C000 SP=FFFE  (16 cyc)
0x0114  22        LD (HL+),A           AF=99B0 BC=3434 DE=00D8 HL=C001 SP=FFFE  (8 cyc)
0x0115  31 FE FF  LD SP,0xFFFE         AF=99B0 BC=3434 DE=00D8 HL=C001 SP=FFFE  (12 cyc)
0x0118  F9        LD SP,HL             AF=99B0 BC=3434 DE=00D8 HL=C001 SP=C001  (8 cyc)
0x0119  08 00 C0  LD (0xC000),SP       AF=99B0 BC=3434 DE=00D8 HL=C001 SP=C001  (20 cyc)
0x011C  76        HALT                 stopped: unimplemented opcode 0x76 at 0x011C
executed 14 instruction(s), PC = 0x011C
```

Everything readable in that table is a property of the program, not of the printout: `B`
becomes `0x34` from `LD B,C`; `HL` walks `C000 → C001 → C000` around the `+`/`-` forms; `A`
holds `0x99` after reading back what was written; `SP` becomes `0xC001` through `LD SP,HL`.
The `sha`-like hex is gone from `AF` because `F8` is not in this program — the flags are
still the boot value `0xB0`.

---

## 7. `tests/cpu_tests.cpp` — 10 new tests

The instruction tests do not poke at `Cpu` internals; they build a **machine** — a synthetic
32 KiB cartridge whose 0x0100 starts with the bytes under test — and run the same path a real
ROM does:

```cpp
struct Machine {
    explicit Machine(const std::vector<uint8_t>& code, uint16_t origin = 0x0100)
        : rom(make_image(code, origin)), bus(), cpu() {
        bus.attach_rom(&rom);
        cpu.attach_bus(&bus);
        cpu.reset();
    }
    static Rom make_image(const std::vector<uint8_t>& code, uint16_t origin);
    Rom rom; Bus bus; Cpu cpu;
};
```

A zero-filled 32 KiB image is a legal cartridge ("ROM ONLY, 32 KiB, no RAM"), so no
hand-written header bytes are needed. Declared in that order so the members construct and die
in the safe order (`rom` outlives `bus`, which outlives nothing it owns).

| Test | Checks |
|---|---|
| `step_without_bus_test` | `step()` on a bus-less CPU throws rather than dereferencing null |
| `nop_test` | 4 cycles, `PC + 1`, flags untouched |
| `ld_r8_imm_and_reg_test` | `8/8/8/4` cycles, immediates land in the right register, `LD B,C` |
| `ld_r16_imm_test` | all four pair loads, little-endian immediate, 12 cycles |
| `ld_indirect_hl_test` | `(HL)` read/write actually reaches WRAM; 12 vs 8 cycles |
| `ld_indirect_bc_de_test` | `(BC)`/`(DE)` as pointers, both directions |
| `ld_hl_inc_dec_test` | access-then-move ordering; `0xFFFF + 1 → 0x0000` |
| `ld_a16_test` | `EA`/`FA`, 16 cycles, round trip through `0xC000` |
| `ld_sp_tests` | `F9`, `08` (20 cycles, low byte first), `F8` flags for `+8` and `-2` |
| `ld_c_indirect_test` | `(C)` addresses `0xFF00 + C`, does not crash on unmapped I/O |
| `unimplemented_opcode_test` | `0xC3` throws and leaves `PC` on the instruction; `0x76` does not become `LD (HL),(HL)` |

All of them assert on **cycle counts as well as values**, which is the point of returning
cycles per instruction: a wrong count is a wrong value once the PPU and timer are ticking.

### The two bugs this day produced

1. **A test that ran off the end of its own program.** `ld_hl_inc_dec_test` used a 4-byte
   program but stepped 5 times; the fifth step executed the zero *after* the program — a
   `NOP`, 4 cycles — and the assert on `8` failed with `m.cpu.hl() == 0xC000`. The emulator
   was right; the test's image was one byte short. Fix: put the fifth instruction in the
   program. Worth keeping in mind: in a zero-filled ROM, running off the end is silent,
   because `0x00` is a legal instruction.
2. **Columns padded with zeroes.** The first trace printed
   `0x0100  0000000000NOP00000000000000000 AF=...`. `std::setfill('0')` from
   `print_rom_header` is *sticky on the stream*, so `std::setw(10)` in the trace padded the
   mnemonics with `'0'`. Fix: `std::setfill(' ')` before the trace. Every `std::hex`,
   `std::setw`, `std::setfill` and `std::left` is a stream-wide setting that outlives the
   function that set it — exactly the class of bug devlog 2 hit with `std::hex`.

---

## 8. Devlog questions

**Why does the CPU need a bus instead of reading the ROM directly?**
Because on this machine memory is not one thing. A read at `0xFF44` is the PPU's scanline
counter, a read at `0xC000` is RAM, a read at `0x4000` may be a *different* ROM bank than the
last read at the same address, and a write at `0x2000` does not store anything at all — it
retargets the mapper. If `Cpu` held a pointer to the `Rom`, then every one of those would end
up as a type check inside the CPU, and each new peripheral would edit the CPU. With a bus,
the CPU has exactly one operation (`read8`/`write8`) and the *address* decides what happens.
That is also what makes the day-3 tests still work: a register file does not care whether
memory exists.

**Why throw on an unimplemented opcode instead of ignoring it?**
Because the alternative is a silent wrong answer that looks like a CPU bug later. If `0xC3`
did nothing and fell through, `PC` would walk into the operand bytes (`37 06` = `SCF`, `LD
B,0x06`) and the machine would execute plausible-looking garbage for a while and then fail
somewhere unrelated, with no record of where it went wrong. Throwing names the opcode and the
address, and restoring `PC` keeps the failure reproducible: the same trace, the same stop.
The cost is that `main` must handle the exception, which is one `catch` in the trace loop.

**Why return cycles per instruction now, when nothing consumes them?**
Because retrofitting a cycle model is much harder than starting with one. The wrong way to do
it is to add `cycles += 4` in the fetch helper and `cycles += 8` for each immediate read:
that works until instruction timing depends on something other than which operands were
fetched — a conditional jump that is 12 cycles taken and 8 not taken, an `LD (HL),r` at 8
cycles where the write overlaps the next fetch — and then the arithmetic in the helpers is a
lie that has to be unpicked case by case. The Pan Docs table gives one number per opcode,
so each case states one number. A missing number is a visible `0`, and the tests assert the
number, so the table is checked every build rather than reviewed once.

**Why is `0x76` a special case in a block that decodes arithmetically?**
Because the hardware reused one opcode slot for `HALT` and left the pattern intact. `0x40`'s
high six bits select "LD r,r'" and the two 3-bit fields select the operands, which makes 63
of those 64 opcodes fall out of a shift and a mask with no table. `0x76` is `LD (HL),(HL)` by
that pattern — a read and a write at the same address, which does nothing — but the real CPU
treats it as `HALT` and stops executing until an interrupt arrives. Since `HALT` needs the
interrupt controller, `0x76` throws today; the important part is that it *does not* silently
become a self-copy, which would look correct in every test that does not watch `PC`.

**Why does `LD (HL+),A` write before incrementing HL?**
Because that is the order the hardware performs. The instruction means "store at the address
in HL, then advance HL", and it exists to walk buffers: `LD (HL+),A` in a loop fills a
contiguous region. Incrementing first would write to `HL + 1` and leave the first byte
untouched — an off-by-one that produces correctly-sized but shifted data, e.g. a sprite that
renders one pixel over. Both `0x22` and `0x2A` are one line each precisely so the order is
visible: `write8(hl(), a()); set_hl(...)`, access first, move second.

---

## 9. What is deliberately still missing

| Gap | Where | Consequence today |
|---|---|---|
| `JP`, `JR`, `CALL`, `RET`, `RST` | `Cpu::execute` | a ROM stops at its second instruction (`C3` at `0x0101`) |
| `ADD`/`SUB`/`AND`/`CP`/`INC`/`DEC` | `Cpu::execute` | no arithmetic, so no flags ever change from `0xB0` |
| `PUSH`/`POP` | `Cpu::execute` | `write16`/`read16` exist; the opcodes do not |
| `HALT` / `IME` / `IF` / `IE` | `Cpu` | `0x76` throws; interrupts unavailable |
| CB-prefix page (256 opcodes) | `Cpu::execute` | `0xCB` throws |
| Cycle accuracy under memory contention | `Cpu` | counts assume no one else is using the bus |
| MBC banking | `Bus` | a 64 KB cart still reads bank 1 at `0x4000-0x7FFF` |
| VRAM/OAM/I-O/HRAM mapping | `Bus` | `LD (0xC000),SP` works, `LD (0xFF80),A` reads back `0xFF` |
| Tests not run by the build | `CMakeLists.txt` | `ctest` is a separate command by hand |

The next step is the one the trace is pointing at: `JP nn` (`0xC3`) plus `JP cc,nn`
(`0xC2`, `0xCA`), then `JR`/`CALL`/`RET`. That turns `cpu_instrs.gb`'s second instruction
from a stop sign into a branch, and the trace starts following the ROM's own entry code
instead of stopping after one line.

---

## 10. How to build and run

```bash
cmake --preset default
cmake --build --preset default

./build/cpu_tests.exe                       # "cpu_tests: all asserts passed", exit 0
ctest --test-dir build --output-on-failure  # same, through CTest: 1/1 passed

./build/gbemu.exe                           # roms/cpu_instrs.gb: NOP, then stops at 0xC3
./build/gbemu.exe build/day4-ld-demo.gb     # the 14-instruction LD trace above
```

The trace runs 16 instructions (`kTraceInstructions`) from `PC = 0x0100`, printing each
instruction's address, bytes, mnemonic and the register file it left behind.

If the build fails with `fatal error C1083: Cannot open include file: 'cstdio'`, VS Code's
CMake Tools has re-pinned `build/CMakeCache.txt` to MSVC; `rm -rf build` and reconfigure
(same caveat as `devlog2.md` §2).
