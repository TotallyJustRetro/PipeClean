# Experimental GBA static-recompilation test

This is an **opt-in Windows-only profile** for testing a separate GBA static recompilation from the PipeClean launcher. PipeClean does not bundle a commercial ROM, ROM-derived generated code, or a BIOS image.

## What the profile provides

The **Superstar Saga (GBA)** profile has dedicated **Game**, **ROM Hacks**, **Display**, **Controllers**, **Bindings**, and **DualSense** tabs. Display options use the GBA's native RGB output; Game Boy Color palettes and LCD/CRT filter presets are intentionally excluded. Keyboard and controller bindings are per-profile, and the selected controller can be identified by its saved GUID/physical identity. DualSense LED colour/brightness and rumble are supported by the external frontend. PipeClean's DualSense speaker test is separate; game audio is not routed to the controller speaker.

The game still renders and runs in a separate Windows process. PipeClean's in-runtime filters, save states, rewind, online play, and Companion Deck do not control the GBA game yet. Sound/hardware behavior and later-game coverage remain experimental.

## Install the compiled frontend

1. Download and extract the PipeClean GBA test package.
2. Keep your legally obtained GBA ROM on your PC. No BIOS file is required by the recompiler's current design; its high-level BIOS implementation is partial.
3. On the GBA profile's **Game** tab, select the ROM and browse to the matching MarioLuigi.exe and overlays.cfg if they're not already at the default locations under gba-recomp.
4. Press **Play**. Closing the GBA window returns to PipeClean.

The starter bindings are arrows to move, **Z** for A, **X** for B, **Enter** for Start, **Backspace** for Select, **A** for L, and **S** for R. Open **Bindings** to change them, or **Controllers** to select a gamepad.

## Adding a compiled GBA ROM hack/profile

GBA ROM hacks need their **own matching static-recompilation executable and overlay configuration**; adding a .gba, .ips, or .bps file alone is not enough. Open **ROM Hacks → Add compiled GBA**, then choose the ROM, the executable compiled for that exact ROM, and its matching overlays.cfg. Select the profile from the library to launch it. The base ROM and base executable settings remain separate.

Each build uses ROM-specific generated code. Keep the generator's work folder private; it contains code derived from the ROM. Don't upload generated C, a game-specific executable built from someone else's ROM, or the ROM itself to the public repository.

## Testing and limitations

- **Display:** Original 3:2, 4:3, or 16:9 shape; pixel-perfect, smooth, stretch, or pixel-stretch scaling; and a configurable startup window size. F11 toggles fullscreen.
- **Controls:** Button bindings and controller identity are passed through a temporary profile config when PipeClean launches the child process. D-pad/left-stick movement is supported.
- **DualSense:** The frontend applies the configured LED and supports short rumble pulses on A/B input. Gameplay event detection and sound output through the controller speaker are not integrated.
- **Performance/audio/graphics:** The frontend's software audio, graphics and timing implementation is experimental. A Windows build that launches does not prove full-game compatibility or consistent frame pacing.
- **BIOS:** The design does not load an original BIOS file, but it implements only a subset of BIOS services. BIOS functionality has not been eliminated.

This branch keeps the GBA launcher integration separate from the built-in Game Boy core. Return to feature/android-pipe-screen-swap to abandon the experiment.