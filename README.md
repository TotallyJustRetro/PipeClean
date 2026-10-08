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

GitHub Actions produces a Windows x64 test build artifact from the same source tree. The ZIP contains the PipeClean executable, the runtime DLL dependencies required by the CI build, and the project documentation.

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

GitHub Actions runs both regressions, validates the Python tools, builds the full native runtime against a synthetic 512 KiB MBC1/SML2-shaped ROM, and performs a headless widescreen launch without shipping any game ROM.

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
