---
description: Build CRML release archives locally and publish tested Windows packages through the tagged GitHub release workflow.
---

# Building releases

Looking to install CRML? Use the [ready-to-use downloads and installation guide](installation.md). The commands below are for contributors building release packages.

Run the release CLI from a Windows x64 checkout with Visual Studio C++ tools and
Python and Edge or Chromium installed. Install the browser test dependency once
with `python -m pip install -r requirements-tests.txt`. Set `VERSION` to the intended version without its `v` prefix,
and add matching release notes under `release/v<version>.md`:

```powershell
.\release.bat
```

The command builds gameplay support with Lua diagnostics disabled, runs the test
suites, checks public text for local paths, and packages the release into
`.local/releases/v<version>/`. Use `-Output <directory>` to choose a new output
location. Existing release directories are preserved; packaging refuses to
overwrite them.

Both native UI browser suites are required release checks. They exercise the
authored settings/feedback page and startup bridge in a headless browser, including
input ownership, page replacement and command receipts. They use local fixtures
and do not launch the game. Missing dependencies or a failed browser check stop
packaging and publication. Use `-Browser <path>` to select a browser executable
when automatic Edge/Chromium discovery does not find it. Native-only `build.bat
-Test` does not run these browser suites.

`-Version` may be supplied explicitly, but must match `VERSION`. The packager
checks the version embedded in both `crml_runtime.dll` and `crml_host.exe`, so an
older build cannot be repackaged with a newer version label. `crml_host --version`
prints the compiled version. Development versions are not published releases.

The SDK includes version-matched documentation listed in `sdk/reference-files.txt`
and the basic C template's explicit source files. Add public reference files to
that inventory deliberately; scratch files and template build output are excluded.
Before publication, the release CLI extracts the SDK ZIP and compiles and runs
its template and maintained C examples using the packaged host.

## Archive contents

| Archive | Contents |
| --- | --- |
| `crml-runtime-…-windows-x64.zip` | Proxy, trusted runtime, Wasmtime, licenses and installation README; no mods or gameplay modes enabled |
| `crml-sdk-…-windows-x64.zip` | Headers, example source, standalone host and WAT compiler |

Each player archive includes installation and removal instructions. Extract game
files beside `CONTROLResonant.exe`, following those instructions. Keep the SDK
outside the game directory. Supported executable fingerprints are recorded in
the release's `compatibility.json`. Other executables require confirmation before
an untested attempt; native hook checks still apply and features may be unavailable.

In framework release notes, lead with the **runtime** download and [installation](installation.md). Individual mods should provide their own feature descriptions, controls and requirements on their distribution pages. SDK downloads belong in mod-author instructions.

`release.json` records the source commit, whether the checkout had uncommitted
changes, the complete supported game fingerprint list (`game_sha256s`), and every
archive/member hash. The legacy `game_sha256` field retains the first fingerprint
for older consumers; it is not the full compatibility list. Packaging checks every
advertised fingerprint against the runtime. Verification also requires inner and
outer profile metadata to agree; removing the outer list cannot downgrade a modern
archive to legacy checks. Genuine older releases without the list remain verifiable
through their recorded hashes.
`SHA256SUMS.txt` lists the archive hashes. The packager uses explicit file lists,
so diagnostic mode markers, logs, saves and debug symbols do not enter a release.
It also rejects embedded paths to the local checkout or user profile in project
files. The unmodified Wasmtime DLL is checked against its pinned upstream hash;
its upstream build paths are preserved.

Verify an existing set without rebuilding:

```powershell
python tools/package_release.py --verify --version v0.1.0-alpha.4.4
```

## Package an example mod separately

Individual mods are distributed separately from framework releases. To prepare the movement example's mod-only ZIP and an optional runtime bundle for a separate distribution, run:

```powershell
.\release.bat -IncludeNoclip -Output .local/releases/example-mod
```

This explicit local option adds `crml-noclip-…-windows-x64.zip` and `crml-noclip-bundle-…-windows-x64.zip` alongside the runtime and SDK. It cannot be combined with `-Publish`. The mod-only archive contains the compiled `examples/movement` package and its mode marker; the bundle also contains the runtime. Edit the example sources and manifest before building a custom variant, and supply that mod's own description, controls and requirements.

The public framework workflow uploads only the runtime and SDK ZIPs plus checksums and metadata. The verifier also accepts older four-archive releases so existing downloads remain verifiable.

## Publishing to GitHub

Add release notes under `release/<version>.md`, commit the reviewed source, and
push that commit to `origin/main`. Then run:

```powershell
.\release.bat -Output .local/publish -Publish
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
