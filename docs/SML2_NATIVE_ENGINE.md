# PipeClean Native SML2 Engine

This track is an independently authored native gameplay implementation. The
existing SM83 interpreter/recompilation path remains available as the behavior
reference and safe fallback while the new engine is developed.

## Ground rules

- Do not copy or link code from third-party SML2 implementations.
- Keep the existing emulator backend working until the native backend passes
  stage-by-stage compatibility checks.
- Do not bundle commercial ROM data or game artwork in source control.
- Use a legally obtained ROM only as a local reference and, where appropriate,
  as input to a private extraction/build step.
- Keep simulation deterministic: one fixed tick, stable player ordering, no
  wall-clock reads or rendering/audio side effects inside gameplay simulation.
- Multiplayer must be a first-class world simulation: each player has an
  independent state, but all players share one level, actor world, collision
  query, and simulation clock.

## Initial implementation

`runtime/sml2_native.c` is the first isolated simulation layer. It currently
provides:

- signed 16.16 fixed-point coordinates and velocities;
- independent player state and simultaneous fixed-tick updates;
- configurable horizontal acceleration/friction, jump impulse, gravity, and
  terminal velocity;
- deterministic axis-separated collision queries against a caller-supplied
  world collision map, plus a native 16×16 block-map adapter with per-block
  SML2 material/interaction flags and explicit out-of-bounds behavior;
- a camera with Off, 16:9, 21:9, and live window-aspect view modes;
- 100%, 125%, 150%, and 200% zoom presets, including the tall-window behavior
  that preserves the native horizontal field while revealing more vertically;
- optional deterministic co-op auto-zoom that picks the smallest zoom-out
  preset needed to frame every active player;
- a shared world-space camera origin that clamps to level edges and centres
  views larger than the level, with extra vertical reveal biased above the
  native playfield;
- player input edge detection for jump and per-player invulnerability timers;
- a ROM-world collision profile that reproduces SML2's 12-bit pixel-coordinate
  wrap, 16×16-cell addressing, and WRAM echo for rows $30-$4D mapping to
  rows $10-$2D. Rows beyond the known map/echo window follow the map's explicit
  out-of-bounds policy.

The ROM-world adapter is based on the reference project's documented
addressing model: the original game masks each camera-coordinate high byte to
four bits, producing a 16×16-screen address space while the ordinary stage
layout occupies 16×3 screens. It also reproduces the documented WRAM echo
window at rows $30-$4D. The native ROM step now runs the source-derived
floor, ceiling, and directional wall probes after the shared fixed-tick
movement update; the probe behavior is still a partial player-physics port.

Movement values are deliberately configurable and provisional. They are not
claimed to reproduce the original game's physics yet. The stage decoder now
classifies SML2's global metatile-ID ranges: `$00-$37` solid/standable,
`$38-$47` standable one-way slope/platform, `$48-$49` coin, `$4A-$4B` exit,
`$4C-$4F` water, `$50-$51` hazard, `$52-$57` moon/low-gravity, and `$58-$7F`
decor. It also marks `$04/$05` bounce, `$05` and `$08-$0F` pipes, `$06`
breakable, and `$7E` hidden-block behavior. Collision queries now preserve the
full material bitmask. The player mover currently reacts only to solid flags;
exact slope geometry, floor-only platform resolution, pickups, water physics,
hazards, breakable blocks, and special interactions still need their own
source-derived simulation paths.

Enemies, moving platforms, bosses, stage transitions, animation, and audio
events are not implemented in this first slice.

## Native stage-data contract

`runtime/sml2_native_stage.h/.c` now defines a renderer- and ROM-independent
stage container for extracted content. A stage supplies a 16×16 metatile grid,
a table mapping each metatile to four 8×8 graphics IDs and SML2 material/interaction flags,
object spawn rectangles, and optional camera-lock rooms. The arrays remain
caller-owned; initialization validates all references and prepares the native
collision-map adapter without copying or taking ownership of game assets.

The stage API provides:
- collision-map flags derived from the metatile material table;
- deterministic row-major visitation of only the requested visible block range,
  returning world coordinates and all four graphics IDs to a renderer callback;
- rectangle-based activation queries for enabled object spawn records, including
  objects just outside the visible view when the caller supplies a wake margin;
- room-ID lookup for camera-lock bounds.

This is the first concrete bridge from the camera's visible-block query into
actual stage content. It is deliberately a data contract and visitor interface,
not a claim that commercial SML2 stage data has already been extracted or that
the renderer/enemy behavior is complete. The next work is a private, legally
sourced extractor and authentic SML2 metatile/collision/object decoding.

### SML2 layout adapter

The stage module also includes a decoder for the ordinary SML2 gameplay map
layout observed by PipeClean's existing compatibility path: 256 columns × 48
rows of 16×16 block IDs (4096×768 world pixels), plus 128 metatile definitions
with four 8×8 graphics IDs each.

The native runtime now also exposes source-derived player probe helpers.
The common `$1EFA` sample reads
`$B000 + ((y-16)>>4)*$100 + ((x-8)>>4)`, including 16-bit coordinate
wrap and the WRAM echo. The first probe helpers reproduce the floor probe's
fractional-Y gate and two sample points, the small/powered ceiling offsets,
and the left/right wall sample heights. The ordered environment probe also
reproduces hazard precedence (powered-player y+22 with y+28 fallback), then
water, exit-bell, and moon-zone classification from the final y+28 sample.
The `$1D75` coin-touch mutation is also implemented: a verified `$48` cell
becomes `$60`, and `$49` becomes `$7F`, with stale/repeated samples rejected.
The ROM step stores the resulting environment flags and sampled tile on each
native player, and uses floor/ceiling/wall hits to update grounded state and
velocity. The mutable-layout step now also reproduces side-probe coin touch:
it rewrites $48/$49 to $60/$7F, increments the per-player BCD coin count
with the original $0999 cap, and requests sound 7. The ordinary read-only
step remains available when the caller must preserve its layout snapshot.
Water/moon physics, hazard damage, and block-bump side effects remain future
work. The stage decoder accepts
caller-supplied memory snapshots and writes into caller-owned native buffers. A verified override table may be supplied for variants; otherwise the decoder
uses SML2's global metatile-ID ranges and special-ID flags.

This is a decoder for supplied stage-memory data, not a ROM parser and not a
claim that stage snapshots are currently being captured automatically from
the game. Homeworld uses a separate 64×64 8×8 tile map and needs its own stage
profile; it must not be misread as an ordinary gameplay room.

## Native stage renderer

`runtime/sml2_native_render.h/.c` renders the native 16x16 metatile stage into
a caller-owned one-byte-per-pixel framebuffer. Each metatile's four 8x8
graphics IDs are decoded through a caller callback and cached once per frame.
The renderer uses the logical camera width and height directly (for example,
256x144 at 16:9) and samples world coordinates from the camera origin; it does
not scale or stretch the original 160x144 emulator image. Out-of-stage pixels
use an explicit palette index. The renderer is SDL-independent so the same
stage output can feed desktop, Android, screenshots, or a future GPU backend.

A Game Boy VRAM adapter is included for validating the original 2-bit tile
bitplanes and LCDC signed/unsigned tile addressing. The graphics callback is
still the seam for a fully ROM-backed native asset decoder; automatic SML2
graphics extraction and the runtime/launcher presentation bridge still need
to be connected.

`runtime/sml2_native_capture.h/.c` adds a read-only live-scene capture
bridge for ordinary SML2 gameplay. It copies the decompressed `$B000-$DFFF`
block map, `$A600-$A7FF` block graphics table, and bank-0 VRAM into caller-owned
storage. Before accepting the snapshot, it checks the verified gameplay
LCDC/window/mode signature and compares the decoded world map against the
current 160×144 PPU tilemap (at least 100 samples and a configurable match
percentage; the intended threshold is 95%). This catches stale RAM, the
Homeworld's different 8×8 map, bonus rooms, and unrelated PPU scenes instead
of treating them as valid stage data. The capture also records the aligned
world origin and BGP register and exposes a helper to apply the active DMG
background palette after rendering.

This is a compatibility/reference capture seam, not the native runtime bridge:
it still reads the live emulator's memory and VRAM. The native game loop, player/actor rendering, and standalone asset extraction
remain unconnected. Basic material/interaction flags are derived from the
verified SML2 metatile-ID ranges when no override is supplied. Exact slope
profiles and the simulation responses to those flags remain future work.

## Native actor lifecycle

`runtime/sml2_native_actor.h/.c` adds a deterministic actor registry for
stage-spawned entities. It materializes spawn records only when their world
rectangles enter the supplied activation bounds, retains stable actor IDs,
sleeps non-persistent actors outside the activation region, retains persistent actors, and updates awake
actors in stable slot order on a fixed simulation tick. It also supports health
damage and a persistent defeated state for unique actors.

The registry is deliberately behavior-agnostic: a game-specific update callback
owns each enemy's movement, attacks, animation state, and special rules. This
keeps the shared world scheduler ready for real SML2 enemy implementations
without inventing behavior before each enemy's original rules are understood.
The current stage spawn records are a generic schema; authentic SML2 spawn-list
decoding and per-enemy callbacks are still required.

## Widescreen and zoom model

The native camera uses a logical world-pixel view rather than stretching a
160×144 image. Its aspect modes are:

- Off: 10:9 (160×144 at 100% zoom).
- 16:9: 256×144 at 100% zoom.
- 21:9: 336×144 at 100% zoom.
- Window: follows the actual window ratio. Very tall windows keep at least the
  native horizontal field and reveal additional vertical space.

Zoom presets are 100%, 125%, 150%, and 200%. For reference, 16:9 at 150% is
384×216, and 21:9 at 200% is 672×288. Co-op auto-zoom can select a wider
preset when players separate, while preserving the selected display aspect.
The camera clamps against the active room and level edges. When a view is
larger than a room, it centres on that room while remaining inside the overall
level; when larger than the entire level, it starts at the level origin rather
than creating negative world coordinates.

The simulation exposes world-to-view conversion, camera world-bounds, rectangle
visibility, and a half-open stage-block range helper. The block-range helper
converts the camera rectangle into the exact 16×16 cells a native stage renderer
should draw, with optional extra cells for prefetch and actor wake-up. Both use
the same camera origin and dimensions. A caller can request the
visible rectangle with a zero margin for rendering/culling, or expand it by a
pixel margin to activate enemies just before they enter the newly visible
edges. Bounds are clamped to the physical level, so edge activation cannot ask
the stage system for negative or beyond-level coordinates.

This follows the public behavior of the reference project at the design level:
expand the logical world view, reconstruct the newly exposed surroundings from
level data, and let object activation account for the expanded edge. PipeClean's
C implementation is independently authored. Actual native stage drawing, level-data extraction, enemy spawning, and object
behavior still need to be ported and connected to these helpers. The new block
range is a renderer contract, not a renderer or stage-data extractor by itself.

The existing emulator-side SML2 widescreen path remains untouched. It rebuilds
level margins from the live decompressed map, validates sampled tiles against
the PPU tilemap, captures world-space actor sprites for the margins, and widens
actor/spawn windows gradually so the game's spawn-list scan does not skip
entries. The native path will instead consume extracted stage data and these
camera bounds; it will not depend on emulator ROM-read hooks or PPU memory taps.

## Integration plan

1. Build a ROM-independent native world and stage-data schema.
2. Finish exact SML2 slope geometry and per-material movement/interaction
   responses, then compare player movement against traces from the emulator.
3. Add actors, enemies, projectiles, damage, power-ups, breakable blocks, and
   stage transitions.
4. Add native rendering/audio event output without binding gameplay to SDL.
5. Integrate two-player co-op, shared-camera policies, lives/respawn, and
   configurable shared/individual progression.
6. Add deterministic snapshots for PipeClean save states and rewind.
7. Run route-by-route compatibility tests before enabling the native backend
   by default. The emulator backend remains the fallback throughout.

## Current status

The native simulation foundation is isolated and unit-tested. It is not yet a
playable SML2 engine and is not connected to the launcher or active gameplay
path.
