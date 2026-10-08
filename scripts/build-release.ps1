# One-command local release build for aimp-discord-presence.
#
# Incremental by default: an existing build/ cache is reused (no --clean-first),
# the Release DLL target rebuilds only what changed, and the offline test binary
# recompiles only when its sources are newer. Pass -Clean to wipe build/ and
# reconfigure from scratch.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Clean
param(
  [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

$timings = [ordered]@{}
function Invoke-Phase([string]$Name, [scriptblock]$Body) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  Write-Host "`n=== $Name ==="
  & $Body
  $sw.Stop()
  $timings[$Name] = $sw.Elapsed
  Write-Host ("--- {0}: {1:mm\:ss\.f}" -f $Name, $sw.Elapsed)
}
function Assert-Ok([string]$What) {
  if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

$BuildDir  = Join-Path $Repo 'build'
$DllPath   = Join-Path $Repo 'output\Release\aimp_discordPresence\x64\aimp_discordPresence.dll'
$TestDir   = Join-Path $BuildDir 'tests'
$TestExe   = Join-Path $TestDir 'cover_publisher_tests.exe'
$Total     = [System.Diagnostics.Stopwatch]::StartNew()

# --- Phase 1: configure (only when stale) ------------------------------------
Invoke-Phase 'configure' {
  if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
    Write-Host 'clean: removed build/'
  }
  $cache = Join-Path $BuildDir 'CMakeCache.txt'
  # AUTOVERSIONING bakes `git rev-list --count HEAD` into the binary at
  # configure time, so a moved HEAD is a stale cache even when CMakeLists
  # files are untouched. A reconfigure never rebuilds by itself; MSBuild
  # still recompiles only what the new defines touch.
  $headFile = Join-Path $BuildDir '.release-head'
  $script:Head = (& git rev-parse HEAD).Trim()
  $configuredHead = if (Test-Path $headFile) { (Get-Content $headFile -Raw).Trim() } else { '' }
  if ((-not (Test-Path $cache)) -or ($configuredHead -ne $script:Head)) {
    & cmake -B $BuildDir -A x64 -DAUTOVERSIONING=ON
    Assert-Ok 'cmake configure'
  } else {
    Write-Host 'configure: cache current for HEAD, skipping (ZERO_CHECK re-runs cmake if CMakeLists changed)'
  }
}

# --- Phase 2: incremental Release build (never --clean-first) ----------------
Invoke-Phase 'build' {
  & cmake --build $BuildDir --config Release --parallel
  Assert-Ok 'cmake build'
  if (-not (Test-Path $DllPath)) { throw "expected DLL missing: $DllPath" }
  Write-Host "dll: $DllPath"
}

# --- Phase 3: offline unit tests (recompile only when sources are newer) -----
Invoke-Phase 'test' {
  $sources = @(
    'tests\cover_publisher_tests.cpp',
    'src\cover_publisher.cpp', 'src\cover_publisher.h',
    'src\album_art.cpp', 'src\album_art.h',
    'src\local_art.cpp',
    'src\presence_layout.h', 'src\utils.h'
  ) | ForEach-Object { Join-Path $Repo $_ }
  $newest = ($sources | ForEach-Object { (Get-Item $_).LastWriteTime } | Measure-Object -Maximum).Maximum
  if (-not (Test-Path $TestExe) -or (Get-Item $TestExe).LastWriteTime -lt $newest) {
    if (-not (Test-Path $TestDir)) { New-Item -ItemType Directory $TestDir | Out-Null }
    # cl needs the MSVC environment; import it once via VsDevCmd.
    # Portable VS discovery: vswhere -latest, with VSINSTALLDIR fallback.
    $vswhere = (Get-Command vswhere -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
    if (-not $vswhere) { $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }
    $vsInstall = if (Test-Path $vswhere) { ((& $vswhere -latest -products * -property installationPath) | Select-Object -First 1).Trim() } else { '' }
    $vsdev = if ($vsInstall) { Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat' } else { '' }
    if ((-not $vsdev) -or (-not (Test-Path $vsdev))) {
      if ($env:VSINSTALLDIR) { $vsdev = Join-Path $env:VSINSTALLDIR 'Common7\Tools\VsDevCmd.bat' }
    }
    if (-not (Test-Path $vsdev)) { throw 'VsDevCmd not found (vswhere -latest + VSINSTALLDIR fallback failed)' }
    $envDump = & cmd /c "`"$vsdev`" -arch=x64 >nul && set" 2>$null
    foreach ($line in $envDump) {
      $i = $line.IndexOf('=')
      if ($i -gt 0) { Set-Item -Path ("env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
    }
    & cl /nologo /std:c++17 /W4 /WX /EHsc `
      "/I$(Join-Path $Repo 'lib\aimp-sdk\Sources\Cpp')" `
      "/I$(Join-Path $Repo 'lib\aimp-glue')" `
      (Join-Path $Repo 'tests\cover_publisher_tests.cpp') `
      (Join-Path $Repo 'src\album_art.cpp') `
      "/Fo$TestDir\\" "/Fe$TestExe" /link winhttp.lib
    Assert-Ok 'cl (test build)'
  } else {
    Write-Host 'test build: exe newer than all sources, skipping recompile'
  }
  & $TestExe
  Assert-Ok 'cover_publisher_tests'
}

# --- Phase 4: version-stamped copy --------------------------------------------
Invoke-Phase 'stamp' {
  $ver = (Get-Content (Join-Path $Repo 'src\product.version') | Where-Object { $_ -match '\S' })
  $buildNum = (& git rev-list --count HEAD).Trim()
  $sha = (& git rev-parse --short HEAD).Trim()
  $version = "$($ver[0]).$($ver[1]).$($ver[2]).$buildNum"
  $stamped = Join-Path (Split-Path $DllPath) "aimp_discordPresence-$version-$sha.dll"
  Copy-Item -Force $DllPath $stamped
  Write-Host "version: $version sha: $sha"
  Write-Host "stamped: $stamped"
  $script:Head | Set-Content (Join-Path $BuildDir '.release-head') -NoNewline
  $script:StampedDll = $stamped
}

$Total.Stop()
Write-Host "`n=== timing summary ==="
foreach ($k in $timings.Keys) {
  if ($timings[$k] -is [timespan]) { Write-Host ("  {0,-10} {1:mm\:ss\.f}" -f $k, $timings[$k]) }
}
Write-Host ("  {0,-10} {1:mm\:ss\.f}" -f 'total', $Total.Elapsed)
Write-Host "`nrelease ok: $($script:StampedDll)"
