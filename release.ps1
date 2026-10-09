param(
    [string]$Version,
    [int]$Jobs = 4,
    [string]$Output,
    [string]$Browser,
    [switch]$Publish,
    [switch]$IncludeNoclip
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$sourceVersion = (Get-Content -LiteralPath "$PSScriptRoot/VERSION" -Raw).Trim()
if (-not $Version) { $Version = "v$sourceVersion" }
if ($Version -cne "v$sourceVersion") { throw 'Release tag must match VERSION. Update VERSION and rebuild for a new release.' }
if ($Version -cnotmatch '^v[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9]+(\.[a-z0-9]+)*)?$') { throw 'Invalid release version.' }
if (-not (Test-Path -LiteralPath "release/$Version.md")) { throw 'Add release notes for this version first.' }
if (-not $Output) { $Output = Join-Path $PSScriptRoot ".local/releases/$Version" }
if (Test-Path -LiteralPath $Output) { throw 'Output already exists; choose a new -Output directory.' }
if ($Publish) {
    if ($IncludeNoclip) { throw 'GitHub releases contain only the runtime and SDK. Package mods locally without -Publish.' }
    $dirty = & git status --porcelain
    if ($LASTEXITCODE -or $dirty) { throw 'Publishing requires a clean, committed working tree.' }
    $releaseCommit = & git rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Cannot read the source commit.' }
    $remoteHead = & git ls-remote --exit-code origin refs/heads/main
    if ($LASTEXITCODE -or ($remoteHead -split '\s+')[0] -ne $releaseCommit) { throw 'Push this reviewed commit to origin/main before publishing.' }
    $existing = & git tag --list $Version
    if ($LASTEXITCODE -or $existing) { throw 'The local release tag already exists or could not be checked.' }
    $remoteTag = & git ls-remote origin "refs/tags/$Version"
    if ($LASTEXITCODE -or $remoteTag) { throw 'The remote release tag already exists or could not be checked.' }
}
# These suites execute the shipped JavaScript in a real headless browser;
# CTest's native protocol tests do not exercise DOM or input ownership behavior.
$browserArgs = @()
if ($Browser) { $browserArgs = @('--browser', $Browser) }
foreach ($suite in @('tests/test_native_ui_page.py', 'tests/test_ui_bootstrap.py')) {
    & python $suite @browserArgs
    if ($LASTEXITCODE) { throw "Browser regression failed: $suite. Install requirements-tests.txt and provide Edge/Chromium (-Browser <path>) if unavailable." }
}
& "$PSScriptRoot/build.ps1" -ExperimentalGameplay -Test -Jobs $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# Use the built host formatter and the shipped tutorial binding together.
# This also checks actual painted aspect ratios rather than DOM bounds alone.
$tutorialFixture = Join-Path $PSScriptRoot '.local/tutorial-release-layout.html'
& "$PSScriptRoot/build/native/Release/crml_tutorial_service_tests.exe" --write-layout $tutorialFixture
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& python tests/test_native_ui_page.py --tutorial-only --tutorial-layout $tutorialFixture @browserArgs
if ($LASTEXITCODE) { throw 'Tutorial image/layout regression failed.' }
& python tools/check_public_paths.py
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$packageArgs = @('--version', $Version, '--output', $Output)
if ($IncludeNoclip) { $packageArgs += '--include-noclip' }
& python tools/package_release.py @packageArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$guestCompiler = Join-Path $PSScriptRoot 'build/deps/wasi-sdk-27.0-x86_64-windows/bin/clang.exe'
& python tests/test_sdk_workflow.py --archive (Join-Path $Output "crml-sdk-$Version-windows-x64.zip") --clang $guestCompiler
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($Publish) {
    # Never commit user work or publish a tag before all local checks pass.
    if ((& git status --porcelain) -or (& git rev-parse HEAD) -ne $releaseCommit) { throw 'Sources changed during the build; publication stopped.' }
    & git tag -a $Version -m "CRML $Version"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & git push origin "refs/tags/$Version"
    if ($LASTEXITCODE) { throw 'Tag push failed; the local tag is retained for inspection.' }
    Write-Host 'Tag pushed. The Release workflow will rebuild, test and publish the prerelease assets.'
}
