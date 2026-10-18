# Tests setup_windows.bat's HelixSR step (the PowerShell part at its end): finding HelixSR's built DLL (Downloads, the
# HelixSR app, typed in, a reinstall's own copy), copying it to HelixSR-fsrfg\amd_fidelityfx_dx12.dll with frame
# generation (helixsr-fg), and an older install's HelixSR folder taken out.
#
#   pwsh helixsr/tests/test_setup_windows.ps1
#
# Runs on Windows (CI); elsewhere the script's \ paths are turned into / so it runs with PowerShell on Linux too.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$bat = [IO.File]::ReadAllText((Join-Path $repo 'setup_windows.bat'))
$ps = $bat.Substring($bat.LastIndexOf('#HSR-PS-START'))
if (-not $IsWindows -and $null -ne $IsWindows) { $ps = $ps.Replace('\', '/') }

$w = Join-Path ([IO.Path]::GetTempPath()) ('hsr-test-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $w | Out-Null
$script = Join-Path $w 'hsr.ps1'
[IO.File]::WriteAllText($script, $ps)
$fails = 0
function Check($name, [bool]$ok) {
    if ($ok) { Write-Host "ok   $name" } else { Write-Host "FAIL $name"; $script:fails++ }
}
function Built($f) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f) | Out-Null
    $b = New-Object byte[] 2080
    (New-Object Random 7).NextBytes($b)
    [Text.Encoding]::ASCII.GetBytes('HXSRNET1').CopyTo($b, 2048)
    for ($i = 2056; $i -lt 2080; $i++) { $b[$i] = 0 }
    [IO.File]::WriteAllBytes($f, $b)
}
function Fg($d) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    foreach ($n in 'nvexec64.dll', '_nvngx.dll', 'nvngx_dlssg.dll', 'libxell.dll') { Set-Content -LiteralPath (Join-Path $d $n) "fg $n" }
    New-Item -ItemType Directory -Force -Path (Join-Path $d '310.9.16') | Out-Null
    Set-Content -LiteralPath (Join-Path $d '310.9.16/data.bin') 'fg data'
    Set-Content -LiteralPath (Join-Path $d 'nvexec.log') 'a log'
}
$pwsh = (Get-Process -Id $PID).Path
function Run($game, [string]$typed = '') {
    New-Item -ItemType Directory -Force -Path $game | Out-Null
    $env:USERPROFILE = Join-Path $w 'home'
    $env:LOCALAPPDATA = Join-Path $w 'local'
    Push-Location $game
    try { $out = $typed | & $pwsh -NoProfile -File $script 2>&1 | Out-String; $code = $LASTEXITCODE } finally { Pop-Location }
    return @{ out = $out; code = $code }
}
function Same($a, $b) { (Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash }

try {
    # the HelixSR download in Downloads, with frame generation
    $dl = Join-Path $w 'home/Downloads/HelixSR-1.9.0'
    Built (Join-Path $dl 'amd_fidelityfx_dx12.dll'); Fg (Join-Path $dl 'helixsr-fg')
    $g1 = Join-Path $w 'g1'
    $r = Run $g1
    Check 'finds HelixSR in Downloads' ($r.code -eq 0 -and (Same (Join-Path $g1 'HelixSR-fsrfg/amd_fidelityfx_dx12.dll') (Join-Path $dl 'amd_fidelityfx_dx12.dll')))
    Check 'copies frame generation' ((Get-ChildItem (Join-Path $g1 'HelixSR-fsrfg/helixsr-fg') -Filter *.dll).Count -eq 4)
    Check "frame generation's data files and subfolders too, not its logs" ((Test-Path (Join-Path $g1 'HelixSR-fsrfg/helixsr-fg/310.9.16/data.bin')) -and -not (Test-Path (Join-Path $g1 'HelixSR-fsrfg/helixsr-fg/nvexec.log')))
    Check 'says frame generation is included, off by default' ($r.out -match 'Frame generation included, off')
    Check 'no HelixSR folder' (-not (Test-Path (Join-Path $g1 'HelixSR')))

    # the HelixSR app's download comes first
    $app = Join-Path $w 'local/HelixSR-Desktop/releases/v1.9.0/HelixSR-1.9.0'
    Built (Join-Path $app 'amd_fidelityfx_dx12.dll')
    $g2 = Join-Path $w 'g2'
    $r = Run $g2
    Check "finds the HelixSR app's" ($r.code -eq 0 -and (Same (Join-Path $g2 'HelixSR-fsrfg/amd_fidelityfx_dx12.dll') (Join-Path $app 'amd_fidelityfx_dx12.dll')))
    Check 'without helixsr-fg: upscaling only' (-not (Test-Path (Join-Path $g2 'HelixSR-fsrfg/helixsr-fg')) -and $r.out -match "isn't set up")

    # nothing found: a wrong file, then HelixSR's folder typed in (frame generation parked by an earlier switch-off)
    Remove-Item -Recurse -Force (Join-Path $w 'home'), (Join-Path $w 'local')
    $else = Join-Path $w 'else/HelixSR'
    Built (Join-Path $else 'amd_fidelityfx_dx12.dll'); Fg (Join-Path $else 'helixsr-fg.off')
    Set-Content -LiteralPath (Join-Path $w 'else/not.dll') 'x'
    $g3 = Join-Path $w 'g3'
    $r = Run $g3 ((Join-Path $w 'else/not.dll') + "`n`"$else`"`n")
    Check 'refuses a wrong file' ($r.out -match "isn't HelixSR's DLL")
    Check 'takes a typed folder' ($r.code -eq 0 -and (Test-Path (Join-Path $g3 'HelixSR-fsrfg/helixsr-fg/nvexec64.dll')))

    $g4 = Join-Path $w 'g4'
    $r = Run $g4 "`n"
    Check 'nothing typed: stops, nothing copied' ($r.code -eq 1 -and -not (Test-Path (Join-Path $g4 'HelixSR-fsrfg')))

    # a reinstall with nothing else around: the game's own HelixSR stays, frame generation too
    $r = Run $g1
    Check "reinstall keeps the game's own" ($r.code -eq 0 -and (Get-ChildItem (Join-Path $g1 'HelixSR-fsrfg/helixsr-fg') -Filter *.dll).Count -eq 4)

    # an install from before 1.1.0: HelixSR\helixsr.dll with helixsr-fg, nothing else around
    $g5 = Join-Path $w 'g5'
    Built (Join-Path $g5 'HelixSR/helixsr.dll'); Fg (Join-Path $g5 'HelixSR/helixsr-fg')
    $r = Run $g5
    Check 'older install: moved to HelixSR-fsrfg, HelixSR folder taken out' ($r.code -eq 0 -and (Test-Path (Join-Path $g5 'HelixSR-fsrfg/amd_fidelityfx_dx12.dll')) -and (Test-Path (Join-Path $g5 'HelixSR-fsrfg/helixsr-fg/nvexec64.dll')) -and -not (Test-Path (Join-Path $g5 'HelixSR')))
} finally {
    Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
}

Write-Host ''
if ($fails -eq 0) { Write-Host 'all passed'; exit 0 } else { Write-Host "$fails failed"; exit 1 }
