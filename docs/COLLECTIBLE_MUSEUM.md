# PipeClean Collectible Museum

The Museum is a shared collection browser with the same layout and discovery rules across games. Each entry has a readable in-app field note, a game-specific identity, and a collection state. Wiki and disassembly research is used to write the descriptions and identify game object IDs; players see the description itself, not a link-only entry.

## Discovery and artwork

- Discoveries are saved to `pipeclean-museum.txt` in PipeClean's settings directory, separately by game ID.
- A supported object's identity comes from the active game's state. Super Mario Land uses its live object table, so a Chibibo/Goombo record is distinct from a Bombshell Koopa, Ganchan, Piranha Plant, and the other mapped species.
- When a mapped Super Mario Land object is visibly rendered for at least two stable frames, PipeClean reconstructs its sprite directly from nearby OAM entries and tile VRAM and saves a native-resolution PNG. This avoids clipping at the viewport boundary and avoids resampling the composited screen. Spawn helpers and ambiguous stomp states do not independently unlock species. Item sprites are cached in their settled form; item entries unlock when the object disappears close to Mario, which remains an inferred pickup signal and should be validated in gameplay.
- Captured assets are private local files at `<settings directory>/pipeclean-museum-assets/<game-id>/<entry-id>.png` (Super Mario Land uses `<entry-id>.v2.png` so old partial crops are ignored). No copyrighted sprites or ROMs are included in PipeClean's repository or releases.
- The UI only displays a captured sprite when the matching game ROM is configured and the entry has been discovered. Undiscovered entries show a lock emblem and a not-yet-unlocked message; the name and field description are withheld until discovery.
- A sprite is not fabricated when the capture fails. The entry displays the Museum's neutral placeholder until PipeClean has usable pixels.

## Current coverage

- **Super Mario Land:** the field guide uses gameplay-focused notes for enemies, hazards, bosses, the Mushroom, 1-Up Heart, Star and Superball Flower. Fly and Kumo share the cartridge's live object type and are described together; Gunion and Tamao have their own records. Local sprite captures still come from the configured ROM; coins use the game event/counter signal and need a separate reliable coin-art capture path.
- Use **Reset all discoveries** in the Museum to clear saved collection progress across every game. The button asks for a second confirmation; local sprite caches and ROM files are kept, and entries must be encountered again to unlock them.
- **Super Mario Land 2:** the Museum catalogue contains 70 individual enemy/boss records (63 regular enemies plus seven bosses, including Wario) alongside coin, power-up, and progression entries. The separate **Enemy tally** milestone uses the game's defeated-enemy counter. Its live tracker reads through the emulated memory bus so the active cartridge RAM bank is respected; it does not depend on an exact HUD-tile match, which could suppress all observations on some display layouts. Individual species still require reliable species-ID mapping before they can unlock separately. A Dev Build records each actor slot's source spawn-descriptor pointer and the six bytes at that descriptor, alongside unique live sprite-table/mapping key combinations, coordinates, and full actor-record bytes, in `pipeclean-sml2-actor-trace.txt` in the settings directory. The game stores the pointer at actor offsets +6/+7; it points to source descriptor byte +2, so the trace also reads the six-byte descriptor from pointer - 2. This source record is a stronger candidate for a stable identity than the changing animation keys, but its fields are still treated as diagnostic evidence until their species meaning is verified. Empty slots reset their per-slot history so a reused slot can yield new evidence. Individual species mappings are not yet complete. The tracker also observes coin collection, life changes, Super/Bunny/Fire forms, the Star timer, secret courses, the final-castle route, and the ending flag.
- **Dr. Mario:** event-based catalogue entries and in-app descriptions; dedicated live-sprite extraction is not yet implemented.
- **Wario Land: Super Mario Land 3:** 15 named treasures (Golden Horn, Harp, Crown, Shield, Bell, Lamp, Crystal Ball, Gold Cup, Dagger, Axe, Conch Horn, Idol, Golden Glove, Whale, and Diamond Ring), each with its course, coin value, and an item description; discovery is driven by the game's treasure flags.
- **Wario Land II (GB/GBC):** 50 treasure flags and 50 picture-panel flags per version. Treasure notes describe the matching-key/chest collection; picture panels come from the number-matching mini-game.
- **Wario Land 3:** 100 named treasure records with level/chest locations and purpose notes where documented, plus key-colour entries. Exact WL3 sprite capture from its game objects remains future work.

## ROM gate and cache format

The Museum never downloads or ships game sprites. To render a discovered specimen's image, it requires both (1) a valid configured ROM for that entry's game and (2) a matching local PNG generated from visible game graphics. Captures are made only during the active matching ROM's emulation. This avoids leaking extracted assets into builds while letting a player's own ROM power their personal Museum.

Descriptions summarize game behavior in original wording, using the corresponding MarioWiki / Wario Land reference pages and the Super Mario Land disassembly as sources for object IDs.


### Wario Land 3 description source
The catalogue uses the game's 100 treasure records with a short in-app explanation of each ability, world-map change, or collection-only item. Descriptions were written from the [MarioWiki treasure list](https://www.mariowiki.com/Wario_Land_III#Treasures) and cross-checked against the [Wario Land 3 treasure guide](https://www.warioland3.com/treasures.html).

## ROM Hacks section

The Museum's **ROM HACKS** view indexes the ROM-hack profiles already registered under each game's Settings > ROM Hacks page. It shows each profile's name, source game, file availability, and whether the profile is selected for launch. ROMs and patches remain local and are never bundled. A collectible catalog specific to a hack still needs object/event mappings for that hack's changed content.
