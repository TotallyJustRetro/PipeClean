# Architecture

PipeClean is split into a toolchain and a native runtime.

## Toolchain

### `tools/sm83.py`
Defines the Game Boy SM83 instruction set and instruction decoding support.

### `tools/dis.py`
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

The repository has a focused hardware-independent SML2 hook regression under `tests/`.

GitHub Actions additionally:

1. builds and runs the SML2 widescreen regression;
2. validates all Python tooling with `py_compile`;
3. generates a synthetic 512 KiB MBC1/SML2-shaped ROM;
4. configures and builds the full native runtime;
5. runs CTest;
6. launches the resulting binary headlessly with the SML2 widescreen option enabled.

No commercial ROM is required for CI.
