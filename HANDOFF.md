# HANDOFF — PipeClean

## Project

PipeClean is a source-first Game Boy recompilation/runtime project covering:

- Dr. Mario
- Super Mario Land
- Super Mario Land 2: 6 Golden Coins

The repository contains the native runtime, toolchain, launcher UI, tests and project documentation. Commercial ROMs are intentionally kept outside the repository.

## Immediate next task — updated Super Mario Land DX widescreen support

**This is the next task to pick up when the user uploads the investigation files from another AI. Do not start speculative ROM-specific edits before inspecting those files and the current code.**

The user has found that the Super Mario Land DX and Super Mario Land 2 DX ROMs previously tested were the wrong source revisions. They have now obtained the correct versions. The launcher now works somewhat with these updated ROMs:

- **Super Mario Land 2 DX:** widescreen works out of the box.
- **Super Mario Land DX (updated ROM hack):** currently does not enable widescreen.
- The user intends to use another AI briefly, upload its findings/files, and then have us continue the implementation.

The immediate goal is to make the *updated Super Mario Land DX ROM hack* work with PipeClean's widescreen option. This is not a request to stretch the picture only: preserve the gameplay behavior that widescreen support is intended to provide, including entity spawning/removal and the expanded visible area.

### Required investigation and implementation approach

1. Read the user's uploaded files and the other AI's findings first. Treat them as evidence to inspect, not as automatically correct.
2. Inspect `runtime/widescreen.c`, `runtime/widescreen.h`, ROM identification/patch handling, and the relevant tests/build workflow.
3. Determine the exact updated SML DX ROM revision and compare its relevant code with the original SML revision expected by the current hook. Use the provided ROM/patch data only as available; do not guess addresses or instruction sequences.
4. Find why `wide_install(GAME_SML, ...)` currently rejects the updated ROM. The existing SML hook checks exact byte sequences at offsets `0x24A5`, `0x24BF`, `0x2584`, and expects 26 zero bytes at `0x3FE4`. The DX hack may have changed these assumptions.
5. Add a separate guarded compatibility path for the updated SML DX revision if needed. Preserve support for the original SML ROM and the working SML2 DX widescreen implementation. Do not weaken the old signature checks globally or patch the user's on-disk ROM; runtime modifications must stay in memory as they do now.
6. Add/update regression tests for the updated SML DX hook and ensure existing SML and SML2 widescreen tests still pass.
7. Build and run tests before claiming success. If the actual ROM is unavailable to automated tests, be explicit about what the synthetic tests prove and what still needs the user's gameplay test.

Do not commit commercial ROMs, ROM hacks, or copyrighted game dumps to the repository. The user can provide relevant patch files, disassembly, hashes, or other permitted test artifacts for analysis; keep proprietary ROM data out of commits and CI artifacts.

## Current state

The previous active target was **Super Mario Land 2 widescreen support**, and that path is working for the user's updated SML2 DX ROM.

### Existing widescreen behavior

The widescreen path is implemented in the runtime:

- PPU output can expand to 256 pixels wide.
- SML2's right-side entity activation bounds are extended at runtime.
- The SML2 hook targets bank 2 address `$401C`, opcode `ADD HL,DE`, and checks related code paths.
- The original activation margins are extended by the configured right-side width.
- SML2 entity scanner bounds are adjusted using runtime hooks.
- Despawn behavior is intentionally left unchanged where documented.
- Runtime hook changes are made in memory; the commercial ROM file is not modified.
- The original SML path adjusts enemy spawn/removal logic through guarded code signatures, but those signatures currently do not enable widescreen for the user's updated SML DX ROM.

## Other recent user-reported status

- The Wario games now launch.
- **Wario Land II GBC** still has a controller-input issue. A controller fallback fix was committed, but the user has not confirmed it works on their actual game/controller yet. Re-test this after the current widescreen task if it remains unresolved.
- The SML and SML2 DX ROMs were initially rejected/not working because the user had the wrong source versions. The user corrected the ROM versions, and the launcher now works somewhat with the updated versions.
- Do not assume the earlier rejection means the ROM hacks are still blocked. Verify behavior against the corrected versions.
- Preserve existing working Dr. Mario behavior and the currently working SML2 DX widescreen path.

## Build and test

Developer build:

```bash
cmake -S . -B build -DROM=/path/to/your/game.gb
cmake --build build --config Release
```

Focused SML2 regression:

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

Check the actual targets in the current Makefile/CMake configuration before assuming these commands have not changed.

## CI

The GitHub Actions workflow is read-only. It does not modify the repository or commit generated files.

CI has been configured to validate:

- SML1 widescreen patch behavior
- SML2 widescreen hook behavior
- MBC1 mapper behavior
- Python tooling
- a synthetic 512 KiB banked-ROM build
- the complete native runtime link
- CTest
- a headless launch of the built runtime with SML2-style widescreen enabled
- a Windows x64 runtime build, smoke test, dependency staging and ZIP artifact

The synthetic ROM is generated during CI and contains only test data; it is not a game dump. Verify the current workflow file and latest run before reporting CI status.

## Important implementation files

`runtime/widescreen.c`  
Widescreen profiles and SML/SML2 compatibility hooks. Primary file for the current task.

`runtime/widescreen.h`  
Widescreen interface and game identifiers.

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

`tools/gbdis.py`  
Small command-line SM83 disassembler, including switchable ROM-bank addressing.

`tests/test_sml2_wide.c`  
Focused SML2 widescreen regression test. Locate and extend the corresponding SML tests for the updated DX path.

## Known limitations

- SML2 gameplay/pop-in verification requires testing with the user's legally obtained ROM in a working SDL2 environment.
- Updated SML DX widescreen compatibility is currently unresolved.
- Wario Land II GBC physical/controller input behavior has not been confirmed fixed by the user.
- Windows-specific audio/device behavior and physical DualSense hardware have not been exhaustively validated here.

## Repository policy

Do not commit:

- commercial ROMs
- ROM hacks
- compiled executables
- SDL runtime DLLs
- generated recompilation output

Keep third-party source/license information documented in `THIRD_PARTY.md`.
