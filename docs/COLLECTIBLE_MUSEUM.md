# PipeClean Collectible Museum

The Museum is a shared, per-game collection in the launcher. It is designed to give every game the same browser, categories, discovery count, and local persistence while leaving the entries and detection rules game-specific.

## Player-facing behavior

- Discoveries are saved immediately to `pipeclean-museum.txt` in PipeClean's settings directory.
- Progress is namespaced by the exact game ID, so Game Boy and Game Boy Color versions don't mix their museum progress.
- Categories include enemies, items, treasures, stages, milestones, and characters. The current catalog only claims a discovery when an existing event signal or a validated cartridge flag can support it.
- Each entry links to a game reference page. Descriptions should be concise summaries, with the reference link shown beside them.
- When a valid ROM is configured for the selected game and the entry has already been discovered, PipeClean may display a matching local sprite image. Without a valid ROM, the sprite is hidden and a neutral original category icon is shown.

## Local sprite assets

Copyrighted sprite art is not distributed with PipeClean. If a user supplies or extracts artwork for a game they have configured, the Museum looks for a PNG at:

```text
<settings directory>/pipeclean-museum-assets/<game-id>/<entry-id>.png
```

Use the exact IDs from `museum_catalog_entry()`, such as `wl3_treasure_001`. The game ID is the `games[].id` value in `runtime/rom.c`. The renderer requires both a valid configured ROM for that game and a discovered entry before it draws the local artwork. The asset file remains on the user's device and is not checked into the repository.

This is currently a local-art lookup convention, not an automatic sprite extractor or downloader. Extracting the correct enemy/item sprite from the ROM, and mapping it reliably to the proper catalog entry, needs game-specific work and gameplay validation.

## Current tracking coverage

- **Dr. Mario:** generic capsule, virus, and clearing events.
- **Super Mario Land:** generic coin, power-up, block, stomp/enemy, and life events.
- **Super Mario Land 2:** coins, defeated-enemy counter, life and extra-life transitions.
- **Wario Land: Super Mario Land 3:** coin/power events plus individual treasure bits.
- **Wario Land II GB / GBC:** 50 treasure bits and 50 picture-panel bits per game version, plus coin/stage/results events.
- **Wario Land 3:** course/key/results event signals, coin/power/treasure changes, four key-colour entries, and a 100-item treasure catalog.

Enemy detections in the Mario games currently unlock a general enemy/virus entry, not every enemy species. Most Wario enemy species and several per-item Mario variants need dedicated per-game object detectors before they can safely unlock individual entries. Wario Land 3's raw last-collected treasure slot also needs confirmation with real-ROM gameplay before the numbered mapping can be considered fully verified.

## References

The catalog includes links to the corresponding MarioWiki pages. The Wario Land 3 treasure names and chest/level notes use a public treasure reference list and should be checked against gameplay before being treated as authoritative data.
