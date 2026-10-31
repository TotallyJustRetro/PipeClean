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
  world collision map;
- a shared camera that expands its horizontal view to frame active players and
  clamps to the configured level bounds;
- player input edge detection for jump and per-player invulnerability timers.

Movement values are deliberately configurable and provisional. They are not
claimed to reproduce the original game's physics yet. The current collision
interface only supplies solid geometry; enemies, breakable blocks, moving
platforms, slopes, hazards, bosses, stage data, animation, and audio events are
not implemented in this first slice.

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
