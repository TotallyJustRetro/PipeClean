# PipeClean native YouGame integration (Windows)

This branch adds the official native C SDK lifecycle to the existing PipeClean Windows
executable. It is separate from the experimental browser/WASM branch.

## Current stage

- Uses YouGame's MIT-licensed native C header, `yougame.h`.
- On Windows builds with `PIPECLEAN_ENABLE_YOUGAME_NATIVE=ON`, PipeClean initializes the
  SDK, pumps `yg_update()` on its main/game loops, reports menu/playing/paused state, and
  exits cleanly when the YouGame app sends its quit instruction.
- When launched by YouGame, the SDK uses the app's native stdin/stdout channel. The game
  reads `YOUGAME_GAME` from the process environment for its listing slug.
- A standalone build can discover the running YouGame app through its local channel once
  `PIPECLEAN_YOUGAME_SLUG` is configured to the published listing slug.

## Not implemented yet

This is the native SDK foundation, **not yet a completed online multiplayer provider**.
PipeClean's existing Render relay remains unchanged. The next stage must implement the
native room/ticket/data transport and connect it to `runtime/netplay.h`, then add the
SML2-specific ROM compatibility and Friends lobby flow.

The current YouGame native desktop handoff documented by YouGame is for GameCube/Wii
discs; it does not document handing a Game Boy ROM to a native Windows build. SML2 ROM
availability inside the Windows sandbox therefore remains an explicit integration issue
to resolve before a hosted listing can launch SML2 automatically.

## Build options

- `PIPECLEAN_ENABLE_YOUGAME_NATIVE`: enable the native SDK on Windows (default ON).
- `PIPECLEAN_YOUGAME_SLUG`: optional fallback listing slug for standalone launches;
  YouGame-launched builds use the app-provided `YOUGAME_GAME` value.

The vendored `yougame.h` is MIT licensed and retains its upstream copyright and license
notice. It was sourced from YouGame's public Melee Unlocked SDK copy:
https://github.com/yougame-co/melee-unlocked-public/tree/main/port/third_party/yougame_start
