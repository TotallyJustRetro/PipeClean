# Experimental GBA static-recompilation test

This is an **opt-in Windows-only handoff test** for the separate GBA static recompiler. It does not replace PipeClean's Game Boy runtime and does not bundle a commercial ROM, ROM-derived generated code, or a BIOS image.

## What this test proves

PipeClean can identify the USA *Mario & Luigi: Superstar Saga* GBA ROM by its header (title `MARIO&LUIGIU`, game code `A88E`) and launch a locally generated `MarioLuigi.exe` as a child process. Closing that window returns to PipeClean and the launcher records the session.

The game still renders and processes input in its own window/process. PipeClean's in-runtime filters, save states, controller bindings, rumble, online play, and Companion Deck do not control the GBA game yet. The recompiler itself has only been exercised through the early part of the game; audio and hardware/BIOS coverage remain incomplete.

## Build on your own Windows PC

1. Keep your legally obtained `Mario & Luigi - Superstar Saga (USA).gba` file outside the repository.
2. Extract the supplied `gba-recomp-local.zip` source to a local folder. Do not copy generated files from another person's ROM build.
3. Run `make_game.bat` and provide the path to your GBA ROM when prompted. The first build may be lengthy; MSYS2/MinGW and Python are required. The script lifts the supplied ROM locally and writes the test executable and overlay profile under its `dist` folder.
4. Beside `PipeClean.exe`, create a folder named `gba-recomp`.
5. Copy only your locally built `MarioLuigi.exe` and matching `overlays.cfg` from the GBA recompiler's `dist` folder into that `gba-recomp` folder. Keep the generated `work` folder private; it contains ROM-derived generated code.

## Test from PipeClean

1. Build PipeClean from this branch on Windows and launch it.
2. Select the **Superstar Saga (GBA)** profile, browse to the USA `.gba` ROM, and press **Play**.
3. The first launch should open a separate 240x160 window. Use arrows to move, **Z** for A, **X** for B, **Enter** for Start, **Backspace** for Select, **A** for L, and **S** for R.
4. Confirm that opening and closing the external window returns cleanly to PipeClean and that the launcher remains usable.

No original BIOS file is required by the GBA recompiler's stated design; BIOS-dependent software interrupts are supplied by its own partial high-level implementation. This does **not** mean all BIOS behavior is implemented or that the game is fully compatible. Do not treat the prototype as a verified all-game GBA solution.

## Reverting / isolation

This test lives on `feature/gba-superstar-saga-pipeclean-test`. Existing Game Boy games continue using their current runtime; the new profile only delegates to the external GBA process. To abandon the prototype, switch back to `feature/android-pipe-screen-swap`.
