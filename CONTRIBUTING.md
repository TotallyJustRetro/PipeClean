# Contributing

Thanks for helping with PipeClean.

PipeClean is a reverse-engineering and runtime project, so focused, testable changes are especially valuable.

## Before you contribute

1. Read [LEGAL.md](LEGAL.md).
2. Never submit commercial ROMs or proprietary game assets.
3. Keep pull requests focused.
4. Add a regression test for reproducible compatibility fixes when practical.
5. Document unusual Game Boy hardware or game-specific assumptions.

## Code style

Prefer clear C, explicit bounds checks, small runtime hooks and comments that explain **why** unusual behavior exists.

Avoid unrelated refactors in compatibility fixes and avoid hard-coded behavior without a signature/address guard.

## Widescreen work

Widescreen is not only a viewport change. Consider:

- camera limits
- object activation
- despawning
- collisions
- HUD placement
- rendering assumptions
- game-specific memory addresses

Game-specific hooks should verify the expected ROM/instruction signature before activating.

## Testing

For the current SML2 widescreen work:

```bash
make test-sml2-wide
```

If your change affects multiple games, test each affected target when possible.

## Pull requests

Please include:

- what changed
- why it changed
- affected game/version
- how it was tested
- known limitations

Logs, screenshots and small reproduction cases are welcome.

## Keep the repository clean

Do not commit:

- ROM files
- save files
- build directories
- compiler output
- `__pycache__/`
- credentials or private keys
- proprietary game assets
