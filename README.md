# PipeClean

![PipeClean logo](assets/pipeclean-logo.svg)

**An independent Game Boy runtime and recompilation project focused on native-scale widescreen, modern controls, and game-specific compatibility work.**

[![Regression Tests](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml)
[![Developer Builds](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml)
[![Android APK](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/android-build.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/android-build.yml)

PipeClean brings classic Game Boy games into a modern runtime with configurable display options, per-game controls, save states, rewind, ROM-hack profiles, and ongoing widescreen work.

> **Project status:** Active development. Some games and features are experimental, and platform support varies by game. This is not yet a polished one-click release.

## Games

PipeClean now has seven game profiles across the Dr. Mario, Super Mario Land, and Wario Land series.

| Game | Current support |
| --- | --- |
| **Dr. Mario** | Core runtime support, game profile, controller settings, and save-state features. The native recompilation path is used when the verified base ROM matches the build; modified game code can require the interpreter/compatibility path. |
| **Super Mario Land** | Widescreen rendering and expanded view, configurable controls, save states, rewind, and suspend. Local co-op is experimental. |
| **Super Mario Land 2: 6 Golden Coins** | MBC1-aware execution for the larger banked ROM, widescreen rendering, and gameplay-aware side-margin composition. The Homeworld has been adjusted to preserve native pixel scale instead of shrinking the full map into a tiny overview. Local and online co-op are experimental. |
| **Wario Land: Super Mario Land 3** | Native-scale widescreen reconstructs level margins from the live block map, with separate guarded visibility adjustments for the clean World ROM and Wario Land DX. |
| **Wario Land II (Game Boy)** | Added as a separate monochrome Game Boy profile. |
| **Wario Land II (Game Boy Color)** | Separate GBC profile; cartridge identification distinguishes it from the original Game Boy release. |
| **Wario Land 3 (Game Boy Color)** | Added as a separate GBC profile. |

### A note about the Wario Land games

The Wario entries remain developmental. **Wario Land: Super Mario Land 3** now reconstructs widened level backgrounds from the live 16x16 block layout rather than repeating the 256px hardware tilemap ring. Its clean World ROM and Wario Land DX profiles use separate guarded object-visibility patches; DX is also recognized from its CGB-only MBC5 header when loaded as a full patched ROM. Adjustments are applied in memory, and unsupported revisions fall back to the original view.

## Highlights

### Native-scale widescreen

Widescreen is more than stretching the original image. PipeClean keeps the original Game Boy picture at native pixel scale and reconstructs supported extra background areas from live game data.

- **Super Mario Land:** expanded view with compatibility hooks for the game's edge-sensitive object handling.
- **Super Mario Land 2:** the native 160-pixel center stays intact while extra level margins are composed from the live decompressed block map.
- **Homeworld:** the wider view no longer scales the entire 512×512 map down into a tiny overview. The map remains at native pixel scale, so widescreen reveals a broader area without making the artwork microscopic.
- **Guarded compatibility:** widescreen hooks check expected game code and map data. If a layout is not recognized, PipeClean can fall back to the normal view rather than blindly applying an unsafe patch.

The widescreen implementation changes runtime behavior; it does not rewrite your original commercial ROM file.

### Launcher and per-game settings

The launcher has a separate profile for each game, along with shared platform settings. Depending on the game, options include:

- Aspect ratio, window size, scaling mode, widescreen amount, palette/color options, and rendering filters.
- Controller selection and per-player bindings, plus configurable emulator shortcuts.
- Save-state slots, load-state support, rewind, and suspend/resume for the built-in runtime.
- Game-specific background artwork and texture-pack support where supported by the built-in renderer.
- A ROM Hacks library that keeps patched variants as separate profiles instead of replacing the original ROM.

ROM hacks can depend on a specific base-ROM revision. If a patch requires a particular revision, PipeClean reports a compatibility error rather than silently assuming it is the right source.

### Android and the dual-screen companion deck

An Android project and debug APK build are included. The launcher has touch support and an opt-in second-screen companion deck for **Super Mario Land** and **Super Mario Land 2**, designed for devices with a second display such as the AYN Thor.

The companion deck includes a large, tappable item circle with a gold pixel ring and a press animation, plus quick actions for save, load, pause/resume, and rewind. Its Background control can load a custom PNG or animated GIF, and the deck also has a Close Game control.

On the AYN Thor's 1080 × 1240 lower display, portrait decks use a responsive 700 × 800 canvas. Companion Deck Studio's portrait preview uses the same proportions, with draggable and resizable layout layers. Landscape displays keep the wide layout.

**Android status:** CI can build the debug APK, but installation, lifecycle behavior, and real-device gameplay still need hands-on verification. Treat Android as a development build for now.

### Local co-op and experimental online co-op

- Local multiplayer foundations are present for Super Mario Land and Super Mario Land 2, with separate player/controller settings. Gameplay and edge cases are still being tuned; consider co-op experimental.
- The development branch contains an experimental relay-based online path for SML1 and SML2. It uses invite codes and checks that both players use the same game and ROM checksum.
- The public relay is not deployed by this repository, and complete Windows-to-Android gameplay has not been verified. Online co-op is not a stable or guaranteed public service.

See [Online Co-op & Relay Setup](docs/ONLINE_NETWORKING.md) for self-hosted/local relay instructions and limitations.

## Downloads

Successful GitHub Actions runs publish test-build artifacts. They are temporary CI artifacts—not permanent GitHub Releases—and they do not include commercial ROMs.

| Platform | Where to get it | Notes |
| --- | --- | --- |
| **Windows x64** | [Regression Tests / runtime build](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml) | ZIP package with the runtime, required DLLs, assets, and docs. |
| **Linux x86-64** | [Regression Tests / runtime build](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml) | `.tar.gz` package; SDL2 and SDL2_image runtime libraries may need to be installed. |
| **Windows / Linux Dev** | [PipeClean Dev Build](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml) | Developer package with additional local-multiplayer diagnostics. |
| **Android debug APK** | [Android APK workflow](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/android-build.yml) | Debug APK for testing, not a store-ready release. Real-device testing is still needed. |

To download an artifact, open a successful workflow run, scroll to **Artifacts**, and download the package for your platform. CI artifacts expire: the standard Windows/Linux packages are retained for 30 days, Dev diagnostics/builds have their own shorter retention, and the Android APK artifact is retained for 14 days.

## Getting started

PipeClean needs a Game Boy or Game Boy Color ROM that you are legally entitled to use. **ROM images are not included in this repository.**

The easiest path is to download a successful build artifact above, extract it, and launch PipeClean. Choose a ROM from its game's launcher profile. PipeClean identifies the cartridge/game from the ROM header and may require a supported ROM revision for specific hacks or compatibility hooks.

For source builds, requirements and full platform instructions are in **[Installation & Build](INSTALL.md)**.

### Build with CMake

Install Python 3, a C compiler, CMake, and the SDL2 / SDL2_image development packages first. Then from the repository root:

```sh
cmake -S . -B build -DROM="/path/to/your/game.gb"
cmake --build build --config Release
```

Or use Make:

```sh
make ROM=roms/your_game.gb
make run ROM=roms/your_game.gb
```

See [INSTALL.md](INSTALL.md) for Windows/Linux dependencies, headless runs, troubleshooting, and additional developer options.

## Tests and developer diagnostics

PipeClean has focused regression tests for the widescreen hooks, cartridge banking, and PPU rendering. The CI regression workflow also checks the relay/WebSocket bridge, validates Python tools, builds the runtime against synthetic test ROMs, and runs headless smoke tests.

Run the focused tests locally:

```sh
make test-sml2-wide
make test-sml-wide
make test-cart-mbc1
```

Expected key messages include `SML2 widescreen hook: PASS` and `MBC1 mapper: PASS`.

### PipeClean Dev diagnostics

The Windows and Linux **PipeClean Dev** builds can open a diagnostics window during local multiplayer. They record frame-by-frame input, player positions, camera/level values, sprite counts, hashes, and noteworthy gameplay events. Screenshots and related memory snapshots help investigate visual glitches, player-spawn issues, camera behavior, or multiplayer regressions. Press **F10** during a session to request an immediate diagnostic snapshot.

Diagnostics are written locally beside the executable under `logs/`. The regular build does not open the developer window or collect the extra multiplayer diagnostics.

For a scripted SML2 multiplayer replay and its limitations, see [Architecture](docs/ARCHITECTURE.md) and `tests/scenarios/sml2_gameplay_template.txt`.

## Project layout

| Path | Purpose |
| --- | --- |
| `runtime/` | Game Boy execution, cartridge/mapper handling, PPU/APU, input, rendering, and launcher |
| `tools/` | Disassembly, instruction analysis, and recompilation utilities |
| `tests/` | Widescreen, cartridge, PPU, networking, and replay regressions |
| `docs/` | Architecture and experimental online-networking documentation |
| `android/` | Android Studio project, setup scripts, and native build integration |
| `server/` | Experimental TCP relay and WebSocket bridge |
| `roms/` | Suggested local location for your own ROMs; no ROMs are shipped |
| `INSTALL.md` | Build and installation instructions |
| `LEGAL.md` | Legal and project policy |
| `THIRD_PARTY.md` | Third-party notices |

## Legal

PipeClean is an independent project and is **not affiliated with, sponsored by, authorized by, or endorsed by Nintendo** or the owners of the games mentioned here.

No commercial ROMs or proprietary game assets are distributed by this repository. You are responsible for obtaining and using game images lawfully. See [Legal & Project Policy](LEGAL.md) and [Third-Party Notices](THIRD_PARTY.md).

---

**PipeClean — classic Game Boy behavior, modern runtime.**
