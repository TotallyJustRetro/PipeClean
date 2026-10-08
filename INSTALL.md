# Installation & Build

PipeClean is currently a development project. A tested Windows x64 build is also produced by GitHub Actions, but PipeClean is not yet a polished one-click release.

## Requirements

### Windows

- Windows 10 or newer
- Python 3
- CMake
- MinGW-w64 or another supported C compiler
- SDL2 **development** package

### Linux

- Python 3
- GCC or Clang
- CMake or GNU Make
- SDL2 development package

### SDL2 note

A runtime `SDL2.dll` is **not** enough to compile PipeClean. You need the SDL2 development package with headers and linker/import files.

## Windows CI build

Each successful GitHub Actions run produces a PipeClean-Windows-x64 artifact. It contains PipeClean.exe, the Windows runtime DLLs used by the build, and the project documentation. The CI package uses a synthetic test cartridge only; commercial ROMs are never included.

For development builds, use the normal CMake instructions below.

## Build with CMake

From the repository root:

```bash
cmake -S . -B build -DROM=/path/to/your/game.gb
cmake --build build --config Release
```

## Build with Make

```bash
make ROM=roms/your_game.gb
```

Run:

```bash
make run ROM=roms/your_game.gb
```

## Super Mario Land 2

SML2 uses a larger banked ROM. PipeClean detects ROMs larger than 32 KiB and selects the interpreter-compatible path rather than treating the cartridge as a flat ROM.

The widescreen implementation runs inside the runtime and does not patch the ROM file.

## Headless / developer runs

The command-line runtime can be used without opening the launcher:

```bash
build/gb --headless --frames 120 /path/to/game.gb
```

Useful development switches include:

- `--interp` — force interpreter execution
- `--wide 100` — enable the configured widescreen profile at 100%
- `--hash` — report a deterministic frame-hash chain and CPU state
- `--dump-ppm FILE` — save the final rendered frame
- `--dump-ram FILE` — save WRAM, HRAM and cartridge RAM
- `--no-crc-check` — load an unrecognized ROM in interpreter mode for development/testing

SML2 can therefore be exercised with its real ROM through the headless path while keeping the ROM outside the repository.

## Testing

```bash
make test-sml2-wide
```

Expected:

```
SML2 widescreen hook: PASS
```

## Troubleshooting

**CMake cannot find SDL2**  
Install the SDL2 development package and make sure CMake can locate it.

**ROM not found**  
Check the `ROM=` path. ROMs are intentionally not included in Git.

**I only have SDL2.dll**  
That DLL is for running an existing Windows program. It does not provide the development headers/libraries needed to compile.

**SML2 behaves incorrectly**  
SML2 support is still being developed. Include your toolchain, build command and relevant log output when reporting a problem.

## Legal

Only use ROM images you are legally entitled to use. PipeClean does not distribute commercial ROMs.
