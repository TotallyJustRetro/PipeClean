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
  solid flags and explicit out-of-bounds behavior;
- a camera with Off, 16:9, 21:9, and live window-aspect view modes;
- 100%, 125%, 150%, and 200% zoom presets, including the tall-window behavior
  that preserves the native horizontal field while revealing more vertically;
- optional deterministic co-op auto-zoom that picks the smallest zoom-out
  preset needed to frame every active player;
- a shared world-space camera origin that clamps to level edges and centres
  views larger than the level, with extra vertical reveal biased above the
  native playfield;
- player input edge detection for jump and per-player invulnerability timers.

Movement values are deliberately configurable and provisional. They are not
claimed to reproduce the original game's physics yet. The initial native tile-map adapter treats 16×16 block IDs as collision
materials and supports solid/out-of-bounds flags. It is a foundation, not the
full SML2 collision table: slopes, one-way platforms, breakable blocks, moving
surfaces, and special hazards still need their original rules ported.

The current collision interface only supplies solid geometry; enemies,
breakable blocks, moving platforms, slopes, hazards, bosses, stage data,
animation, and audio events are not implemented in this first slice.

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

The simulation exposes world-to-view conversion and rectangle visibility
helpers so the upcoming world renderer and actor-activation system use the
same camera origin and dimensions. This lets off-native-screen enemies and
terrain be considered consistently. Actual stage drawing, enemy spawning,
and object behavior still need to be ported and connected to these helpers. The original emulator's current widened-PPU path remains
separate until the native renderer has full stage coverage.

## Integration plan

1. Build a ROM-independent native world and stage-data schema.
2. Implement original SML2 collision/block-map semantics and compare movement
   against traces from the existing emulator.
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
