# DockX packaging script
# -----------------------------------------------------------------------------
# Builds BOTH release artifacts from the freshly built dist\ folder:
#   1. DockX-<version>-windows-x64-Installer.exe  (via Inno Setup)
#   2. DockX-<version>-windows-x64.zip            (manual / advanced install)
# Both land in release\ at the repo root.
#
# The version is read from buildspec.json -- the single source of truth. This
# script never hard-codes it.
#
# Prereqs:
#   - dist\dockx.dll and dist\locale\en-US.ini must be the current build
#     (they are refreshed as part of every build; verify the version first).
#   - Inno Setup 6 installed for the .exe (https://jrsoftware.org/isdl.php).
#     If ISCC is not found, the .zip is still produced and the .exe is skipped
#     with a note.
#
# Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File installer\package.ps1
# -----------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

# Resolve repo root as the parent of this script's folder.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$distDir   = Join-Path $repoRoot 'dist'
$relDir    = Join-Path $repoRoot 'release'

# --- version from the single source of truth ---
$spec = Get-Content (Join-Path $repoRoot 'buildspec.json') -Raw | ConvertFrom-Json
$version = $spec.version
if (-not $version) { throw 'Could not read version from buildspec.json' }
Write-Host "DockX version (from buildspec.json): $version" -ForegroundColor Cyan

# --- sanity: dist must exist and match ---
$dll = Join-Path $distDir 'dockx.dll'
if (-not (Test-Path $dll)) { throw "Missing $dll -- build first (cmake --build ...) then refresh dist\." }

New-Item -ItemType Directory -Force -Path $relDir | Out-Null

# --- 1. ZIP (advanced / manual install) ---
# Stage a clean tree, then compress it. Contents: DLL, locale, the batch
# helpers, and a README explaining install + the SmartScreen warning.
$stage = Join-Path $env:TEMP ("dockx-zip-" + $version)
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'locale') | Out-Null
Copy-Item $dll                                   (Join-Path $stage 'dockx.dll')
Copy-Item (Join-Path $distDir 'locale\en-US.ini') (Join-Path $stage 'locale\en-US.ini')
Copy-Item (Join-Path $distDir 'install.bat')      (Join-Path $stage 'install.bat')
Copy-Item (Join-Path $distDir 'uninstall.bat')    (Join-Path $stage 'uninstall.bat')
Copy-Item (Join-Path $scriptDir 'README.txt')     (Join-Path $stage 'README.txt')

$zipPath = Join-Path $relDir "DockX-$version-windows-x64.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath
Remove-Item $stage -Recurse -Force
Write-Host "  built: $zipPath" -ForegroundColor Green

# --- 2. EXE installer (Inno Setup) ---
$iscc = $null
foreach ($p in @(
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
  if (Test-Path $p) { $iscc = $p; break }
}
if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }

if ($iscc) {
  & $iscc "/DMyAppVersion=$version" (Join-Path $scriptDir 'dockx.iss')
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
  Write-Host "  built: $relDir\DockX-$version-windows-x64-Installer.exe" -ForegroundColor Green
} else {
  Write-Host "  SKIPPED .exe: Inno Setup (ISCC.exe) not found." -ForegroundColor Yellow
  Write-Host "  Install it from https://jrsoftware.org/isdl.php, then re-run this script." -ForegroundColor Yellow
}

Write-Host "`nDone. Artifacts in: $relDir" -ForegroundColor Cyan
