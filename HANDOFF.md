# HANDOFF — PipeClean

## Project

PipeClean is a source-first Game Boy recompilation/runtime project covering:

- Dr. Mario
- Super Mario Land
- Super Mario Land 2: 6 Golden Coins

The repository contains the native runtime, toolchain, launcher UI, tests and project documentation. Commercial ROMs are intentionally kept outside the repository.

## Current state

The primary active target is **Super Mario Land 2 widescreen support**. Super Mario Land 1 widescreen is already implemented and regression-tested.

SML2 is a 512 KiB MBC1 cartridge, so its current path uses the full interpreter rather than pretending the cartridge is a flat 32 KiB image.

The widescreen path is implemented in the runtime:

- PPU output can expand to 256 pixels wide.
- SML2's right-side entity activation bound is extended at runtime.
- The hook targets bank 2 address `$401C`, opcode `ADD HL,DE`.
- The original `DE=$0060` activation margin is extended by the configured right-side width.
- The hook refuses to run when the expected bank/opcode is not present.
- Despawn behavior is intentionally left unchanged.
- The commercial ROM file is never modified.

## Build and test

Developer build:

```bash
cmake -S . -B build -DROM=/path/to/your/game.gb
cmake --build build --config Release
```

Focused regression:

```bash
make test-sml2-wide
```

Expected:

```
SML2 widescreen hook: PASS
```

Headless development example:

```bash
build/gb --headless --frames 120 --wide 100 /path/to/your/game.gb
```

## CI

The GitHub Actions workflow is now read-only. It does not modify the repository or commit generated files.

CI validates:

- SML1 widescreen patch behavior
- SML2 widescreen hook behavior
- MBC1 mapper behavior
- Python tooling
- a synthetic 512 KiB banked-ROM build
- the complete native runtime link
- CTest
- a headless launch of the built runtime with SML2-style widescreen enabled

The synthetic ROM is generated during CI and contains only test data; it is not a game dump.

## Important implementation files

`runtime/widescreen.c`  
Widescreen profiles and SML/SML2 compatibility hooks.

`runtime/ppu.c`  
DMG rendering, widened output buffer and widescreen gating.

`runtime/cart.c`  
Cartridge header parsing and MBC1/MBC3/MBC5 banking.

`runtime/rom.c`  
ROM identification/loading and ROM-hack handling.

`runtime/emu.c`  
Frame execution, developer/headless controls and runtime orchestration.

`runtime/main.c`  
Command-line entry point and developer switches.

`tools/recomp.py`  
SM83 static recompiler/interpreter generator.

`tests/test_sml2_wide.c`  
Focused widescreen regression test.

## Known limitations

Full SML2 gameplay/pop-in verification still requires running the project with a legally obtained SML2 ROM on a machine with a working SDL2 environment.

Windows-specific audio/device behavior and physical DualSense hardware have not been exhaustively validated here.

## Repository policy

Do not commit:

- commercial ROMs
- ROM hacks
- compiled executables
- SDL runtime DLLs
- generated recompilation output

Keep third-party source/license information documented in `THIRD_PARTY.md`.
