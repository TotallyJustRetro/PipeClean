# PipeClean native YouGame integration (Windows)

This branch adds the official native C SDK lifecycle to the existing PipeClean Windows
executable. It is separate from the experimental browser/WASM branch.

## Current stage

- Uses YouGame's MIT-licensed native C header, `yougame.h`, and retains its adjacent
  upstream `LICENSE`.
- On Windows builds with `PIPECLEAN_ENABLE_YOUGAME_NATIVE=ON`, PipeClean initializes the
  SDK, pumps `yg_update()` on its main/game loops, reports menu/playing/paused state, and
  exits cleanly when the YouGame app sends its quit instruction.
- When launched by YouGame, the SDK uses the app's native stdin/stdout channel and reads
  the listing slug from `YOUGAME_GAME`.
- The root `yougame-native.json` declares version 1 and `"net": "internet"` for this
  transition build. This preserves the existing Render relay while allowing the YouGame
  native connection. Once PipeClean's netplay provider uses the native room broker end to
  end, switch the manifest to `"net": "yougame"` to remove direct network access.
- A standalone build can discover the running YouGame app through its local channel once
  `PIPECLEAN_YOUGAME_SLUG` is configured to the published listing slug.

## Not implemented yet

This is the native SDK and desktop-launch foundation, **not yet a completed online
multiplayer provider**. PipeClean's current `runtime/netplay.c` still uses the existing
Render relay; the YouGame broker transport has not yet been connected to
`runtime/netplay.h`. The next stage must implement ticket issuance and Friends/queue
selection, room/lobby messages, game start/results, and the SML2 ROM compatibility flow.
A green Windows build proves compilation and the configured smoke test, not that a
two-player session works.

The native facade now also exposes generic broker primitives for the next adapter:
`yougame_native_request()` wraps SDK `yg_call()` for requests such as `mpTicket`,
and delivers replies from `yougame_native_update()`. SDK push events can be forwarded
through `yougame_native_set_push_handler()`, and raw non-call lines through
`yougame_native_set_line_handler()`. `yougame_native_next_request_id()` and
`yougame_native_write_line()` are available to a protocol client that must share the
SDK's request counter and stdio channel. These callbacks run on the PipeClean thread
that pumps the SDK. Reply strings (especially `result_json`) are only valid for the
duration of the callback, so an adapter must copy any data it retains. This is request/event plumbing only: it is not a lobby or matchmaking implementation.

A separate C façade, `runtime/yougame_native_net.h/.c`, now wraps the native
broker's `netOpen`, `netSend` and `netClose` calls. It JSON-escapes text safely,
checks queue room-code syntax, validates `pass` (24 UTF-16 code units), `name`
(40 UTF-16 code units) and close-reason length (123 UTF-8 bytes), parses the returned
connection id, and forwards `net` pushes. Pushes may arrive before the `netOpen`
answer; the future room client must buffer those events until it associates the
connection id. This wrapper does not yet request `mpTicket` or implement room
protocol, matchmaking, the Friends picker, or PipeClean lockstep messages.

The current published native handoff schema explicitly documents GameCube/Wii disc
placeholders. YouGame's recent updates also mention setup of players' own SNES and DS
copies for native builds, but the SDK's documented native placeholders do not describe
a Game Boy/Game Boy Color ROM handoff. SML2 ROM availability inside the Windows sandbox
therefore remains an integration issue to verify with YouGame before a hosted listing
can launch SML2 automatically.

## Build options

- `PIPECLEAN_ENABLE_YOUGAME_NATIVE`: enable the native SDK on Windows (default ON).
- `PIPECLEAN_YOUGAME_SLUG`: optional fallback listing slug for standalone launches;
  YouGame-launched builds use the app-provided `YOUGAME_GAME` value.

The vendored `yougame.h` is MIT licensed and retains its upstream copyright and license
notice. It was sourced from YouGame's public Melee Unlocked SDK copy:
https://github.com/yougame-co/melee-unlocked-public/tree/main/port/third_party/yougame_start
