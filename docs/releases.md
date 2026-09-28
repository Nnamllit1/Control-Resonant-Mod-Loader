---
description: Build CRML release archives locally and publish tested Windows packages through the tagged GitHub release workflow.
---

# Building releases

Looking to install CRML? Use the [ready-to-use downloads and installation guide](installation.md). The commands below are for contributors building release packages.

Run the release CLI from a Windows x64 checkout with Visual Studio C++ tools and
Python installed:

```powershell
.\release.bat -Version v0.1.0-alpha.3
```

The command builds gameplay support with Lua diagnostics disabled, runs the test
suites, checks public text for local paths, and packages the release into
`.local/releases/v0.1.0-alpha.3/`. Use `-Output <directory>` to choose a new output
location. Existing release directories are preserved; packaging refuses to
overwrite them.

## Downloads

| Archive | Contents |
| --- | --- |
| `crml-runtime-…-windows-x64.zip` | Proxy, trusted runtime, Wasmtime, licenses and hello mod; no gameplay mode enabled |
| `crml-noclip-…-windows-x64.zip` | Wasm movement mod and its mode marker; requires the matching runtime |
| `crml-noclip-bundle-…-windows-x64.zip` | Runtime and noclip together |
| `crml-sdk-…-windows-x64.zip` | Headers, example source, standalone host and WAT compiler |

Each player archive includes installation and removal instructions. Extract game
files beside `CONTROLResonant.exe`, following those instructions. Keep the SDK
outside the game directory. Only the executable fingerprint recorded in the
release's `compatibility.json` is supported by gameplay operations.

`release.json` records the source commit, whether the checkout had uncommitted
changes, the supported game fingerprint, and every archive/member hash.
`SHA256SUMS.txt` lists the archive hashes. The packager uses explicit file lists,
so diagnostic mode markers, logs, saves and debug symbols do not enter a release.
It also rejects embedded paths to the local checkout or user profile in project
files. The unmodified Wasmtime DLL is checked against its pinned upstream hash;
its upstream build paths are preserved.

Verify an existing set without rebuilding:

```powershell
python tools/package_release.py --verify --version v0.1.0-alpha.3
```

## Publishing to GitHub

Add release notes under `release/<version>.md`, commit the reviewed source, and
push that commit to `origin/main`. Then run:

```powershell
.\release.bat -Version v0.1.0-alpha.3 -Output .local/publish-v0.1.0-alpha.3 -Publish
```

`-Publish` requires a clean checkout matching `origin/main` and an unused version
tag. It builds and checks locally before creating and pushing the annotated tag.
It does not commit files or push the main branch. A failed tag push leaves the
local tag available for inspection.

The tag triggers `.github/workflows/release.yml`. GitHub rebuilds and tests on
Windows, verifies the downloaded archive hashes, uploads a draft release, then
publishes it as a prerelease after the upload succeeds. The workflow uses the
repository's `GITHUB_TOKEN` with write access limited to the publishing job. See
the [GitHub release CLI reference](https://cli.github.com/manual/gh_release_create).
If publishing fails after draft creation, inspect the draft and workflow logs
before retrying; existing release assets are never silently overwritten.

Before publishing a gameplay release, check it in a playable save: movement in
each direction, passage through geometry, return to normal control, Insert while
flight is active, focus loss, and reload. Automated rendering and lifecycle tests
do not replace that game check.
