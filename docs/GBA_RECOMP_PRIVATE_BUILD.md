# Private GBA cloud build (no local compiling)

The `.github/workflows/gba-private-build.yml` workflow builds the GBA static-recompiler executable on a temporary GitHub-hosted Windows runner. It does not add a ROM, generated C, the executable, or the overlay configuration to the repository.

## Required repository Actions secrets

Open **Settings → Secrets and variables → Actions → New repository secret** and add:

- `GBA_RECOMP_SOURCE_URL`: a direct HTTPS download URL for the `gba-recomp-local.zip` source archive. The URL must let the runner download the ZIP without logging in interactively.
- `GBA_ROM_URL`: a direct HTTPS download URL for your own compatible `Mario & Luigi: Superstar Saga (USA).gba` ROM. Do not commit the ROM or put it in a public repository/release.
- `GBA_OUTPUT_KEY`: a unique strong passphrase used to encrypt the resulting package. Keep it private; without it the downloaded build cannot be extracted.

The URL values are stored as Actions secrets and the workflow does not intentionally print them. A link that grants access to anyone who possesses the link is still a bearer link; do not reuse it elsewhere. Prefer a provider's expiring direct-download link when available. The download endpoint must be reachable by GitHub's hosted runner.

## Run it

1. Configure all three secrets.
2. Open the **Actions** tab and select **Private GBA Recompiler Build (Encrypted)**.
3. If the first run says it is waiting for secrets, open that run and choose **Re-run all jobs** after saving the secrets.
4. When successful, download the `encrypted-gba-recompiler-build` artifact. It contains only `gba-recomp-output.7z`, protected with the `GBA_OUTPUT_KEY` passphrase.
5. Open the `.7z` file with 7-Zip and enter the same passphrase. The extracted files are `MarioLuigi.exe`, `overlays.cfg`, and a short README. Put the executable and overlays file together under `PipeClean/gba-recomp/`.

Artifacts expire after three days. The ROM and intermediate generated source are only used on the temporary build runner and are not uploaded as artifacts. GitHub still processes the ROM during the build, so this removes it from the repository—not from the cloud runner's temporary workspace.

## Build limitations

This is still an experimental, ROM-specific static recompiler. The ROM is required **at build time** to lift instructions and profile runtime-loaded overlays; it is not bundled in the output. This workflow reproduces the source archive's existing one-pass profiling recipe and may need further profiling if the game reaches code not covered by the seed/profile run. A successful compile is not proof that every later scene works.

The workflow deliberately publishes no unencrypted executable or generated C because those files are derived from a commercial ROM. Only a password-encrypted archive is retained by the Actions artifact service.
