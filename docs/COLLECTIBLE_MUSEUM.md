# PipeClean Collectible Museum

The Museum is a shared collection browser with the same layout and discovery rules across games. Each entry has a readable in-app field note, a game-specific identity, and a collection state. Wiki and disassembly research is used to write the descriptions and identify game object IDs; players see the description itself, not a link-only entry.

## Discovery and artwork

- Discoveries are saved to `pipeclean-museum.txt` in PipeClean's settings directory, separately by game ID.
- A supported object's identity comes from the active game's state. Super Mario Land uses its live object table, so a Chibibo/Goombo record is distinct from a Bombshell Koopa, Ganchan, Piranha Plant, and the other mapped species.
- When a mapped Super Mario Land object is visible, PipeClean crops its real sprite pixels from the frame the emulator has just rendered and writes the PNG to the user's local cache. Enemies enter the collection when recognized. Item sprites can be cached while visible; item entries unlock when the object disappears close to Mario, which is an inferred pickup signal and should be validated in gameplay.
- Captured assets are private local files at `<settings directory>/pipeclean-museum-assets/<game-id>/<entry-id>.png`. No copyrighted sprites or ROMs are included in PipeClean's repository or releases.
- The UI only displays a captured sprite when the matching game ROM is configured and the entry has been discovered. Uncollected entries retain their mystery-slot art, while their researched description remains readable in the detail pane.
- A sprite is not fabricated when the capture fails. The entry displays the Museum's neutral placeholder until PipeClean has usable pixels.

## Current coverage

- **Super Mario Land:** mapped object IDs for the enemies, hazards, boss objects, mushroom, heart, star and Superball Flower; sprite capture is performed from the active rendered frame. Coins still use the game event/counter signal and need a separate reliable coin-art capture path.
- **Super Mario Land 2 and Dr. Mario:** current event-based catalogue entries and in-app descriptions. Dedicated per-species live-sprite extraction is not yet implemented.
- **Wario Land: Super Mario Land 3:** 15 treasure records (A–O), with course/value notes and live collection flags.
- **Wario Land II (GB/GBC):** 50 treasure flags and 50 picture-panel flags per version. Treasure notes describe the matching-key/chest collection; picture panels come from the number-matching mini-game.
- **Wario Land 3:** 100 named treasure records with level/chest locations and purpose notes where documented, plus key-colour entries. Exact WL3 sprite capture from its game objects remains future work.

## ROM gate and cache format

The Museum never downloads or ships game sprites. To render a discovered specimen's image, it requires both (1) a valid configured ROM for that entry's game and (2) a matching local PNG generated from visible game graphics. Captures are made only during the active matching ROM's emulation. This avoids leaking extracted assets into builds while letting a player's own ROM power their personal Museum.

Descriptions summarize game behavior in original wording, using the corresponding MarioWiki / Wario Land reference pages and the Super Mario Land disassembly as sources for object IDs.


### Wario Land 3 description source
The catalogue uses the game's 100 treasure records with a short in-app explanation of each ability, world-map change, or collection-only item. Descriptions were written from the [MarioWiki treasure list](https://www.mariowiki.com/Wario_Land_III#Treasures) and cross-checked against the [Wario Land 3 treasure guide](https://www.warioland3.com/treasures.html).