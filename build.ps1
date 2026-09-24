param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Test, [int]$Jobs = 1)
$ErrorActionPreference = 'Stop'
& python "$PSScriptRoot\tools\fetch-deps.py"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (-not $vs) { throw 'Install Visual Studio with Desktop development with C++.' }
$cmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
$generator = if (([version]$vs.installationVersion).Major -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
& $cmake -S $PSScriptRoot -B "$PSScriptRoot\build\native" -G $generator -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build "$PSScriptRoot\build\native" --config $Configuration --parallel $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($Test) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    & $ctest --test-dir "$PSScriptRoot\build\native" -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
& $cmake --install "$PSScriptRoot\build\native" --config $Configuration --prefix "$PSScriptRoot\dist"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
New-Item -ItemType Directory -Force "$PSScriptRoot\dist\crml\mods\hello" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\examples\hello\mod.ini" -Destination "$PSScriptRoot\dist\crml\mods\hello\mod.ini"
& "$PSScriptRoot\dist\crml\crml_wat.exe" "$PSScriptRoot\examples\hello\hello.wat" "$PSScriptRoot\dist\crml\mods\hello\hello.wasm"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "Build ready in $PSScriptRoot\dist"
