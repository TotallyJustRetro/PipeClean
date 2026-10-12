# PipeClean

![PipeClean logo](assets/pipeclean-logo.svg)

**An independent Game Boy runtime and recompilation project bringing classic games to modern displays with widescreen rendering, configurable controls, and game-specific compatibility work.**

[![Regression Tests](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml)
[![Developer Builds](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml/badge.svg)](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml)

> **Status: Active development.** PipeClean is a developer/test project, not yet a polished one-click release. Support varies by game and some features are experimental.

## Games

PipeClean's game profiles cover seven entries across three classic series.

| Game profile | Status |
| --- | --- |
| **Dr. Mario** | Native recompilation/runtime support for the verified base ROM, with a compatibility/interpreter path for cases that cannot use the compiled build. |
| **Super Mario Land** | Widescreen support, configurable presentation and controls, and ongoing gameplay compatibility work. |
| **Super Mario Land 2: 6 Golden Coins** | Widescreen support for the larger banked ROM, with runtime hooks for wider object activation and background margins. Homeworld rendering is being refined to keep its pixels crisp at native scale. |
| **Wario Land: Super Mario Land 3** | Game profile added; compatibility and integration work is ongoing. |
| **Wario Land II (Game Boy)** | Separate monochrome GB profile. |
| **Wario Land II (Game Boy Color)** | Separate GBC profile; ROM identification distinguishes it from the monochrome release. |
| **Wario Land 3 (Game Boy Color)** | Separate GBC profile. |

The four Wario entries are newer additions and should be treated as developmental rather than assumed to have feature parity with the more established SML/SML2 paths.

## Features

### Native-scale widescreen

The original Game Boy picture is 160 pixels wide. PipeClean can expand the viewport, but the objective is to reveal more of the world—not simply stretch or shrink the original pixels.

- **Super Mario Land:** widened presentation with compatibility hooks for edge-sensitive gameplay.
- **Super Mario Land 2:** additional level margins are reconstructed from live map data, and the runtime extends selected object-activation bounds so extra visible space can participate in gameplay.
- **Homeworld work:** the latest development branch removes a zoomed-out overview that made the full map appear tiny, returning the map to native pixel scale while retaining the wider viewport. This fix is still on the development branch pending integration into the default branch.
- **Guarded compatibility:** widescreen hooks check expected game code/map layouts. Unsupported scenes can use the normal view rather than blindly applying an unsafe patch.

The widescreen implementation changes runtime behavior; it does not rewrite your original commercial ROM file.

### Launcher and runtime options

Depending on the game and runtime path, PipeClean includes:

- Configurable aspect ratio, scaling, window size, and widescreen amount.
- Game-specific controller configuration and emulator shortcut bindings.
- Save states, load states, rewind, and suspend/resume for the built-in runtime.
- Rendering palettes/filters, custom launcher backgrounds, and texture-pack support where supported.
- A ROM-hack library that keeps patched variants as separate profiles rather than replacing the original ROM.
- Regression tests for widescreen hooks, cartridge banking, and PPU rendering.

ROM hacks can require a specific clean-ROM revision. Follow any compatibility message shown by PipeClean when loading a patch.

## Latest development work

The latest platform and companion-screen work is being developed on [`feature/online-android-foundation`](https://github.com/TotallyJustRetro/PipeClean/tree/feature/online-android-foundation), tracked in [draft pull request #8](https://github.com/TotallyJustRetro/PipeClean/pull/8). It is separate from the default branch until the work is reviewed and merged.

That branch includes:

- An Android Studio project and debug-APK build workflow.
- Touch input and an opt-in dual-screen companion deck for Super Mario Land and Super Mario Land 2, designed with devices such as the AYN Thor in mind.
- A larger tappable item circle with an animated press effect, save/load/pause/rewind controls, a Close Game action, and custom PNG or animated GIF backgrounds.
- Experimental local and relay-based online co-op foundations for Super Mario Land and Super Mario Land 2, plus updated networking and developer diagnostics.
- Ongoing Homeworld widescreen refinement to keep the map crisp instead of zoomed out.

These branch features are **experimental**. The public relay is not deployed by this repository, complete Windows-to-Android gameplay has not been verified, and an Android CI build does not replace testing on a real device. Follow the branch and pull request links above for the newest implementation and build checks.

## Downloads

GitHub Actions publishes temporary test-build artifacts when its workflows complete successfully. These are not permanent GitHub Releases, and they do not contain commercial game ROMs.

- **Windows x64 / Linux x86-64:** use the [Regression Tests workflow](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/regression.yml), open a successful run, then download the relevant artifact.
- **Developer diagnostics build:** use the [PipeClean Dev Build workflow](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/dev-build.yml). The Dev build adds diagnostics intended for investigating local multiplayer.
- **Android debug APK:** currently built on the experimental development branch. Use the [Android APK workflow in that branch](https://github.com/TotallyJustRetro/PipeClean/actions/workflows/android-build.yml?query=branch%3Afeature%2Fonline-android-foundation).

CI artifacts expire after their configured retention period. Download and keep any build you want to preserve.

## Getting started

You need a Game Boy or Game Boy Color ROM that you are legally entitled to use. **No commercial ROMs are included in this repository.**

For a prebuilt test version, download a successful Windows or Linux artifact above, extract it, and open PipeClean. Select your own ROM in the appropriate game profile.

For source builds, read **[Installation & Build](INSTALL.md)**. At a minimum, a build requires Python 3, a C compiler, CMake or Make, and the SDL2 / SDL2_image development packages.

Build with CMake:

```sh
cmake -S . -B build -DROM="/path/to/your/game.gb"
cmake --build build --config Release
```

Or use Make:

```sh
make ROM=roms/your_game.gb
make run ROM=roms/your_game.gb
```

See [INSTALL.md](INSTALL.md) for full dependencies, Linux runtime requirements, headless runs, and troubleshooting.

## Testing and diagnostics

Run the focused regression tests locally:

```sh
make test-sml2-wide
make test-sml-wide
make test-cart-mbc1
```

Expected messages include `SML2 widescreen hook: PASS` and `MBC1 mapper: PASS`.

The Regression Tests workflow also checks PPU rendering, networking utilities, Python tools, runtime builds, and headless smoke tests using synthetic test ROMs. Those synthetic ROMs are test fixtures, not playable commercial game content.

The PipeClean Dev builds include additional multiplayer diagnostics such as frame-by-frame inputs, player/camera values, hashes, event-triggered screenshots, and memory snapshots. Those files are written locally in a `logs/` directory to help developers investigate gameplay issues. Diagnostics are not collected by the regular runtime.

See [Architecture](docs/ARCHITECTURE.md) for implementation details and [Online Co-op & Relay Setup](https://github.com/TotallyJustRetro/PipeClean/blob/feature/online-android-foundation/docs/ONLINE_NETWORKING.md) for the experimental branch's networking notes.

## Project layout

| Path | Purpose |
| --- | --- |
| `runtime/` | Game Boy execution, mapper handling, PPU/APU, input, rendering, and launcher |
| `tools/` | Disassembly, instruction analysis, and recompilation utilities |
| `tests/` | Widescreen, mapper, PPU, networking, and replay regressions |
| `docs/` | Architecture and technical notes |
| `roms/` | Optional local location for your own ROMs; no ROMs are shipped |
| `INSTALL.md` | Build and installation instructions |
| `LEGAL.md` | Legal and project policy |
| `THIRD_PARTY.md` | Third-party notices |

## Legal

PipeClean is an independent project and is **not affiliated with, sponsored by, authorized by, or endorsed by Nintendo** or the owners of the games mentioned here.

No commercial ROMs or proprietary game assets are distributed by this repository. You are responsible for obtaining and using game images lawfully. See [Legal & Project Policy](LEGAL.md) and [Third-Party Notices](THIRD_PARTY.md).

---

**PipeClean — classic Game Boy behavior, modern runtime.**
