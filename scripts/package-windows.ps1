# Builds release packages into dist\ on Windows, from a plain PowerShell (no Developer Prompt needed):
#   Clipper-<version>-windows-x64.zip        portable, unzip and run
#   Clipper-<version>-windows-x64-setup.exe  Inno Setup installer (per user, no admin)
#   Clipper-<version>-windows-x64.msi        WiX installer (per user, no admin)
#   SHA256SUMS-windows
#
#   .\scripts\package-windows.ps1            # release build + tests + packages
#   .\scripts\package-windows.ps1 -NoBuild   # package the existing build\release
# Needs Visual Studio 2022 (C++ workload, comes with cmake + ninja), Inno Setup 6
# (winget install JRSoftware.InnoSetup) and WiX 5 (dotnet tool install --global wix --version 5.0.2,
# then wix extension add -g WixToolset.UI.wixext/5.0.2 WixToolset.Util.wixext/5.0.2).
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

$cmake = Get-Content CMakeLists.txt -Raw
$version = [regex]::Match($cmake, 'project\(Clipper VERSION ([0-9.]+)').Groups[1].Value
$version += [regex]::Match($cmake, 'set\(CLIPPER_VERSION_SUFFIX "([^"]*)"\)').Groups[1].Value  # 1.0.0 / 1.1.0-beta
$numeric = $version -replace '-.*$', ''  # msi / file versions are numbers only
$name = "Clipper-$version-windows-x64"
$dist = Join-Path $PWD 'dist'

if (-not $NoBuild) {
    # the x64 msvc environment, imported into this session
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found' }
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    cmake --preset release
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build --preset release
    if ($LASTEXITCODE) { throw 'build failed' }
    ctest --test-dir build\release --output-on-failure
    if ($LASTEXITCODE) { throw 'tests failed' }
}
$exe = 'build\release\Clipper.exe'
if (-not (Test-Path $exe)) { throw "no release build at $exe" }

if (Test-Path $dist) { Get-ChildItem $dist | Remove-Item -Recurse -Force }  # not the folder: explorer may have it open
$stage = Join-Path $dist $name
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item LICENSE (Join-Path $stage 'LICENSE.txt')
@"
Clipper $version
$('=' * (8 + $version.Length))

Everything you copy, one shortcut away. Clipboard history for Windows and Linux.

Run Clipper.exe. It sits in the tray (next to the clock) and remembers what you copy:
text, images and files. Press Ctrl+Shift+V in any app for the popup at your mouse,
type to search, Enter pastes. The first start also turns on "Start Clipper when I
log in", you can turn that off in Preferences.

This folder is the portable version, nothing gets installed. Your history lives in
%LOCALAPPDATA%\Clipper, settings in %APPDATA%\Clipper\settings.ini.
Start-at-login is the "Clipper" entry in Task Manager > Startup apps.

Project, help and updates: https://github.com/KimsFerdy/Clipper
MIT license, see LICENSE.txt. Uses GLFW (zlib), Dear ImGui (MIT), stb (public domain / MIT).
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'README.txt')

$zip = Join-Path $dist "$name.zip"
Compress-Archive -Path $stage -DestinationPath $zip
"Built $zip"

$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) { $iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) { throw 'Inno Setup not found (winget install JRSoftware.InnoSetup)' }
& $iscc /Q "/DAppVersion=$version" "/DAppVersionNumeric=$numeric" "/DSourceDir=$stage" "/DOutputDir=$dist" `
    "/DOutputName=$name-setup" installer\Clipper.iss
if ($LASTEXITCODE) { throw 'Inno Setup failed' }
"Built $dist\$name-setup.exe"

$wix = (Get-Command wix -ErrorAction SilentlyContinue).Source
if (-not $wix) { $wix = "$env:USERPROFILE\.dotnet\tools\wix.exe" }
if (-not (Test-Path $wix)) { throw 'WiX not found (dotnet tool install --global wix --version 5.0.2)' }
Push-Location installer  # the .wxs paths (icon, License.rtf) are relative to it
try {
    & $wix build Clipper.wxs -arch x64 -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext -d "Version=$numeric" `
        -d "SourceDir=$stage" -o "$dist\$name.msi"
    if ($LASTEXITCODE) { throw 'WiX failed' }
} finally { Pop-Location }
Remove-Item -Force -ErrorAction SilentlyContinue "$dist\$name.wixpdb"
"Built $dist\$name.msi"

Remove-Item -Recurse -Force $stage
# same format as sha256sum on linux, so it can go next to (or into) the linux SHA256SUMS
Push-Location $dist
try {
    $sums = Get-ChildItem -File | Where-Object Name -NotLike 'SHA256SUMS*' | Sort-Object Name | ForEach-Object {
        '{0}  ./{1}' -f (Get-FileHash $_.Name -Algorithm SHA256).Hash.ToLower(), $_.Name
    }
    # \n line ends: `sha256sum -c` on linux chokes on \r\n
    [IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS-windows'), (($sums -join "`n") + "`n"))
} finally { Pop-Location }
Get-ChildItem $dist | Format-Table Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } } -AutoSize
