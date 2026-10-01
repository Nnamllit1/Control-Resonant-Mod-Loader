param(
    [string]$Version = 'v0.1.0-alpha.4.2',
    [int]$Jobs = 4,
    [string]$Output,
    [switch]$Publish,
    [switch]$IncludeNoclip
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
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
& "$PSScriptRoot/build.ps1" -ExperimentalGameplay -Test -Jobs $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& python tools/check_public_paths.py
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& python tests/test_release.py
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$packageArgs = @('--version', $Version, '--output', $Output)
if ($IncludeNoclip) { $packageArgs += '--include-noclip' }
& python tools/package_release.py @packageArgs
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
