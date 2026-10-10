# PipeClean

**A source-first Game Boy recompilation project with modern widescreen support.**

[![Regression Tests](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml)

PipeClean is an independent project exploring static recompilation and a lightweight native runtime for classic Game Boy software.

The current focus is bringing **Super Mario Land 2: 6 Golden Coins** to a modern PC presentation while preserving the original game's behavior as closely as possible.

> **Status:** 🛠️ Active development

## ✨ What is PipeClean?

PipeClean combines static recompilation tools, a native Game Boy runtime, modern PC rendering/input, widescreen presentation work, and targeted compatibility fixes for larger banked ROMs.

The goal is not to rewrite the original games. The runtime is designed around the original machine behavior, with carefully guarded fixes where modern presentation requires them.

## 🎯 Current focus — Super Mario Land 2

SML2 is the primary widescreen target.

Its larger **MBC1 banked ROM** uses an interpreter-compatible execution path. The widescreen implementation also adjusts the game's right-side object-activation boundary, so the additional visible area can actually participate in gameplay instead of being purely stretched graphics.

## 🕹️ Supported / planned

| Game | Status |
| --- | --- |
| Super Mario Land 2: 6 Golden Coins | 🛠️ Active |
| Super Mario Land | ✅ Widescreen implemented |
| Dr. Mario | 🔬 Development |

## 📁 Project layout

| Path | Purpose |
| --- | --- |
| `runtime/` | Native Game Boy runtime |
| `tools/` | Recompiler, disassembler and analysis tools |
| `tests/` | Regression tests |
| `docs/` | Technical documentation |
| `roms/` | Local ROM location — ROMs are not included |
| `CMakeLists.txt` | CMake build |
| `Makefile` | Make build |

## 🚀 Getting started

PipeClean is currently a **developer/test build**, not a finished one-click release.

See **[Installation & Build](INSTALL.md)**.

You'll need:

- Python 3
- A C compiler
- CMake or Make
- SDL2 **development** files
- A ROM you are legally permitted to use

**Commercial ROMs are not distributed by this repository.**

## 🖥️ Widescreen

The original Game Boy display is **160 pixels wide**.

PipeClean expands the presentation while also considering game simulation. For SML2, the runtime intercepts the specific object-activation calculation responsible for the original right-side boundary and extends it for the wider view.

The hook is tightly guarded and does not modify the commercial ROM file.

## 🪟 Windows build

GitHub Actions produces a Windows x64 test build artifact from the same source tree. The ZIP contains the PipeClean executable, its runtime DLL dependencies, the PipeClean icon/assets, and the project documentation.

The artifact is built and smoke-tested against a synthetic test cartridge. No commercial ROM is included. To run a real game, supply a ROM you are legally permitted to use.

## 🧪 Testing

Run the focused SML2 widescreen regression test:

```bash
make test-sml2-wide
```

Expected result:

```
SML2 widescreen hook: PASS
```

The repository also has dedicated SML1 and MBC1 regressions:

```bash
make test-sml-wide
make test-cart-mbc1
```

Expected result:

```
MBC1 mapper: PASS
```

GitHub Actions runs the regressions, validates the Python tools, builds the full native runtime against synthetic ROMs, and runs headless smoke tests without shipping any game ROM.

### PipeClean Dev multiplayer diagnostics

The separate **PipeClean Dev** Windows build opens a live diagnostics window whenever local multiplayer starts. It records per-frame inputs, player positions, camera/level state, sprite counts, coins, tilemap/world-memory hashes and a rendered-frame hash. Events such as lives changes, spawn changes, likely movement stalls and sprite-list warnings are recorded too. Closing only the diagnostics window hides it but keeps logging; leaving the multiplayer session finalizes the log.

Logs are written beside the executable in `logs/PipeClean-dev-multiplayer-YYYYMMDD-HHMMSS-*.jsonl`. The regular PipeClean build does not open this developer window or create these session logs. Download **PipeClean-Dev-Windows-x64** from the separate [PipeClean Dev Build workflow](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml).

### Scripted multiplayer replay test

The headless test runner can execute repeatable two-player SML2 input sequences and write a JSONL diagnostic record for every logical frame. Each record includes player coordinates/grounding, camera and level values, sprite counts, tilemap/world-memory hashes, and a render hash. The wrapper replays the same inputs twice and fails if the recorded frames differ.

With a local SML2 ROM, build PipeClean and run:

```bash
cmake -S . -B build -DROM="path/to/your/Super Mario Land 2.gb"
cmake --build build --target PipeClean
python3 tools/mp_test.py \
  --binary build/PipeClean \
  --rom "path/to/your/Super Mario Land 2.gb" \
  --script tests/scenarios/sml2_gameplay_template.txt \
  --frames 1800 \
  --report-dir test-results/sml2-coop
```

Edit the script's frame timings to reach the level and reproduce the interaction you want to test. Input masks are hexadecimal: A=01, B=02, Select=04, Start=08, Right=10, Left=20, Up=40, Down=80. You can add assertions when the scenario is expected to reach gameplay, for example `--require-p2-spawn --require-p1-movement --require-p2-movement --require-camera-scroll`; use `--require-block-patch` to require that the runtime recorded a tile change, or `--require-actor-region-change` to detect activity in the actor-related RAM range. These are useful checks, not proof by themselves that the underlying game behavior is correct. Reports are saved locally and are not uploaded by the test tool. The CI runner uses a synthetic cartridge only to validate scripted stepping and deterministic replay; it does **not** prove that collisions, blocks, or enemy behavior are correct in the commercial game.

See **[Architecture](docs/ARCHITECTURE.md)** for technical details.

## ⚖️ Legal

PipeClean is independent and is **not affiliated with, sponsored by, authorized by, or endorsed by Nintendo**.

No commercial ROMs or proprietary game assets are included. Users are responsible for obtaining and using ROMs lawfully.

See **[Legal & Project Policy](LEGAL.md)**.

## 🤝 Contributing

Code, testing, reverse-engineering notes, bug reports and compatibility improvements are welcome.

Read **[Contributing](CONTRIBUTING.md)** before submitting changes.

---

**PipeClean**  
*Classic Game Boy behavior. Modern runtime.*
