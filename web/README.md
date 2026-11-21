# PipeClean YouGame Web Prototype

This is an experimental Emscripten build path on `feature/yougame-online-prototype`.
It does not replace the normal Windows or Android apps, or their existing Render relay.

## Build

Use a current Emscripten SDK with CMake:

```sh
emcmake cmake -S . -B build-yougame-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-yougame-web --parallel
```

The output is a static web folder containing `index.html`, the JavaScript/WASM runtime,
and a preloaded `assets.data` package. Upload the **contents** of that folder as the
YouGame web build.

## ROM handling and listing setup

The package contains only a synthetic build-time ROM used by the interpreter generator.
It contains no playable Game Boy ROM, extracted game assets, or Nintendo artwork.

The hosted listing must be a YouGame **patch** listing for Super Mario Land 2. Before
testing the hosted build, register the accepted base-game dump with YouGame using the
official base-game catalog and dump data. The page then obtains the player's copy through
`YouGame.baseGame()`; the browser runtime accepts only a ROM that PipeClean identifies
as Super Mario Land 2. The base ROM is never uploaded by this project.

The exact accepted dump SHA-1 and size have deliberately not been guessed or embedded here.
They must come from YouGame's catalog and the copy the creator has permission to use.
The base SHA-1 is part of the matchmaking compatibility key, so players with different
dumps cannot enter the same lockstep room.

When opened outside the YouGame frame, the web shell exposes a local file picker for
development. It is not used by the published listing.

## Current prototype scope

- One supported game: Super Mario Land 2.
- Two remote players in private YouGame Friends rooms.
- Mario/P1 is the first match connection; Luigi/P2 is the second.
- The existing PipeClean deterministic, frame-numbered input exchange is adapted to
  YouGame room messages with the existing eight-frame delay.
- ROM CRC verification happens before starting.
- YouGame owns room invitations, player identity, room membership, voice and Leave UI.
- PipeClean owns emulation, game rules, player assignment and simulation.

Shared online save slots are intentionally unavailable in this first web provider. The
existing local save data remains local; account-backed binary save integration is a later
milestone. Ranked matchmaking is intentionally out of scope for cooperative play.

## Important limitations

This is a prototype build, not a production release. Verify a two-account staging
session, real friend invitation, matching-ROM rejection, SML2 input determinism,
disconnect behavior, pause/resume, browser refresh, save behavior and phone layout
before publishing. The web pilot does not enable GBA external-player launch or the
native Companion Deck simulator.
