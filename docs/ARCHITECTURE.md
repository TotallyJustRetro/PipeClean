# Architecture

PipeClean is split into a toolchain and a native runtime.

## Toolchain

### `tools/sm83.py`
Defines the Game Boy SM83 instruction set and instruction decoding support.

### `tools/gbdis.py`
Provides a small command-line disassembler for inspecting Game Boy machine code, including switchable ROM banks.

### `tools/discover.py`
Helps discover executable regions and references that are useful when preparing a recompilation.

### `tools/recomp.py`
Performs the static recompilation/lifting stage. Discovered SM83 code is translated into C for the native runtime. Banked cartridges can instead be generated in interpreter-only mode.

## Runtime

The `runtime/` directory contains the native execution environment:

- SM83 CPU/interpreter support
- cartridge and mapper handling
- memory, timers and interrupts
- DMG PPU/video behavior
- APU/audio
- controller input
- native rendering and launcher UI
- widescreen compatibility hooks
- ROM-hack and texture-pack support

Generated game code stays separate from the runtime so platform and presentation work can evolve independently.

## Cartridge handling

PipeClean distinguishes between small ROM-only cartridges and larger banked cartridges.

For the current SML2 path, the 512 KiB MBC1 cartridge is run through the interpreter so the complete ROM remains addressable through mapper bank switching. The build system automatically selects `--interp-only` for ROMs larger than 32 KiB.

## SML2 widescreen

The original Game Boy viewport is 160 pixels wide.

A wider desktop viewport can expose additional world space, but simply drawing more pixels is not enough: game logic may use the original right edge when deciding when to activate objects.

SML2's entity activation routine, in bank 2, reaches address `$401C` with an `ADD HL,DE` instruction. The original `DE` value is `$0060`, representing the original 96-pixel right-side activation margin.

PipeClean intercepts that specific interpreter instruction and evaluates:

    original:    activation_bound = camera + 0x60
    widescreen: activation_bound = camera + 0x60 + right_extension

The hook is guarded by the active ROM bank and expected opcode. The ROM file itself is never rewritten.

Despawning remains separate from activation so the compatibility change does not unexpectedly change entity lifetime on the left side.

## PPU widescreen model

The PPU can render up to 256 pixels wide. The original 160-pixel picture sits inside the wider buffer at `ppu_xoff`.

Per-game profiles provide:

- left extension
- right extension
- optional HUD centering
- optional window centering
- a gate that turns widescreen presentation on only during gameplay

This lets menus, maps and status screens keep their original composition where appropriate.

## Testing

The repository has focused hardware-independent regressions under `tests/` for widescreen hooks, cartridge banking and PPU rendering.

The headless multiplayer test mode accepts an event script with one combined button mask for each of two players at each change point, advances both players through the normal `emu_mp_step` path, and writes one JSONL diagnostic record per logical frame. Reports include player and camera coordinates, grounding/air flags, spawn/visible-sprite data, level/coins, and deterministic hashes of the background map, world RAM, an actor-related RAM region, and the rendered frame. `tools/mp_test.py` runs the same script twice and verifies the frame reports are identical. This tests repeatability and makes regressions easier to localize; it is not an automatic proof that game rules are correct.

GitHub Actions builds the full native runtime and runs this test mode against a synthetic ROM with a minimal VBlank interrupt loop. That checks input replay, frame scheduling, reporting and deterministic reruns without distributing a commercial ROM. For actual collisions, block persistence, enemy AI and level progression, run the harness locally against a legally obtained SML2 ROM with an input script that reaches and exercises the relevant gameplay scenario.
