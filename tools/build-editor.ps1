# Builds the Fast_Godot editor with the toolchain this file lives in.
# The toolchain and the source tree can be anywhere; the source is found from
# -Engine, GRIT_ENGINE/GRIT_SRC, the current directory, the toolchain's parents or
# common locations.

$ErrorActionPreference = "Continue"
$toolchain = $PSScriptRoot
$engine = ""
$extra = @()

$i = 0
while ($i -lt $args.Count) {
    $a = $args[$i]
    if ($a -eq "-Toolchain" -and $i + 1 -lt $args.Count) { $toolchain = $args[$i + 1]; $i += 2; continue }
    if ($a -eq "-Engine" -and $i + 1 -lt $args.Count) { $engine = $args[$i + 1]; $i += 2; continue }
    $extra += $a
    $i++
}

function Find-Engine {
    param([string]$Start, [string]$Toolchain)
    $candidates = @()
    if ($env:GRIT_ENGINE) { $candidates += $env:GRIT_ENGINE }
    if ($env:GRIT_SRC) { $candidates += $env:GRIT_SRC }
    if ($Start) { $candidates += $Start }
    $candidates += (Get-Location).Path
    $candidates += $Toolchain
    foreach ($c in $candidates) {
        $p = [IO.Path]::GetFullPath($c)
        for ($n = 0; $n -lt 4 -and $p; $n++) {
            if (Test-Path (Join-Path $p "tools\grit_build.py")) { return $p }
            $parent = Split-Path $p -Parent
            if (-not $parent -or $parent -eq $p) { break }
            $p = $parent
        }
    }
    $roots = @($env:USERPROFILE, (Join-Path $env:USERPROFILE "Desktop"), (Join-Path $env:USERPROFILE "Documents"), (Join-Path $env:USERPROFILE "Downloads"), "C:\b", "D:\b") | Where-Object { $_ -and (Test-Path $_) }
    foreach ($root in $roots) {
        $hit = Get-ChildItem -Path $root -Directory -Depth 2 -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName "tools\grit_build.py") } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return ""
}

if (-not $engine) { $engine = Find-Engine -Start "" -Toolchain $toolchain }
if (-not $engine) {
    Write-Host "[error] No Fast_Godot source tree found (needs tools\grit_build.py)."
    Write-Host "        Pass -Engine DIR or set GRIT_ENGINE."
    exit 1
}

$python = Join-Path $toolchain "python\python.exe"
$script = Join-Path $engine "tools\grit_build.py"
if (-not (Test-Path $python)) { Write-Host "[error] Toolchain not found: $toolchain"; exit 1 }
if (-not (Test-Path $script)) { Write-Host "[error] Build script not found: $script"; exit 1 }

$log = Join-Path $engine "editor_build.log"
Write-Host "Toolchain: $toolchain"
Write-Host "Source:    $engine"
Write-Host "Log:       $log"
Write-Host ""
Write-Host "Building the editor... the first build on a source tree takes a while."
Write-Host ""

& $python $script editor --engine $engine --toolchain $toolchain --jobs 16 @extra 2>&1 | Tee-Object -FilePath $log
$code = $LASTEXITCODE
Write-Host ""
if ($code -eq 0) {
    Write-Host "Build succeeded."
    Write-Host "Editor: $(Join-Path $engine 'bin\godot.windows.editor.x86_64.exe')"
} else {
    Write-Host "Build failed with exit code $code. See $log"
}
exit $code
