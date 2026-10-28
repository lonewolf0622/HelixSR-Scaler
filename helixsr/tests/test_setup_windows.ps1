# Tests setup_windows.bat's HelixSR step (the PowerShell part at its end): finding HelixSR 3.x after its setup (Downloads,
# the HelixSR app, typed in, a reinstall's own copy), HLX DLSS to HelixSR\helixsr.dll with frame generation
# (helixsr-fg), HLX FSR 4 to OptiScaler\ with AMD's FSR as .amd.dll, OptiScaler.ini, and HelixSR 2.0's folder taken out.
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
function Hlx($f, [bool]$built = $true) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f) | Out-Null
    $b = New-Object byte[] 2072
    (New-Object Random 9).NextBytes($b)
    [Text.Encoding]::ASCII.GetBytes('HLX FSR 4').CopyTo($b, 16)
    if ($built) { [Text.Encoding]::ASCII.GetBytes('HLXFSR4D').CopyTo($b, 2048); for ($i = 2056; $i -lt 2072; $i++) { $b[$i] = 0 } }
    [IO.File]::WriteAllBytes($f, $b)
}
# a game with HelixSR Scaler's download extracted into it, as setup_windows.bat runs: AMD's FSR in OptiScaler\
function Game($g) {
    New-Item -ItemType Directory -Force -Path (Join-Path $g 'OptiScaler') | Out-Null
    Set-Content -LiteralPath (Join-Path $g 'OptiScaler/amd_fidelityfx_upscaler_dx12.dll') "AMD's FSR"
    [IO.File]::WriteAllText((Join-Path $g 'OptiScaler.ini'), "[Upscalers]`r`nDx12Upscaler=auto`r`n[DLSS]`r`nHelixSR=auto`r`n[Plugins]`r`nLoadAsiPlugins=auto`r`n[FrameGen]`r`nFGtry {
    # a HelixSR 3.x download in Downloads after its setup: HelixSR\helixsr.dll with frame generation, HLX-FSR4\ built
    $dl = Join-Path $w 'home/Downloads/HelixSR-3.1.0'
    Built (Join-Path $dl 'HelixSR/helixsr.dll'); Fg (Join-Path $dl 'HelixSR/helixsr-fg')
    Hlx (Join-Path $dl 'HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll')
    $g1 = Join-Path $w 'g1'; Game $g1
    $r = Run $g1
    Check 'finds HelixSR in Downloads: HLX DLSS as HelixSR\helixsr.dll' ($r.code -eq 0 -and (Same (Join-Path $g1 'HelixSR/helixsr.dll') (Join-Path $dl 'HelixSR/helixsr.dll')))
    Check "HLX FSR 4 (built) in OptiScaler\, AMD's FSR as .amd.dll" ((Same (Join-Path $g1 'OptiScaler/amd_fidelityfx_upscaler_dx12.dll') (Join-Path $dl 'HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll')) -and (Get-Content -Raw (Join-Path $g1 'OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll')) -match "AMD's FSR")
    Check 'copies frame generation' ((Get-ChildItem (Join-Path $g1 'HelixSR/helixsr-fg') -Filter *.dll).Count -eq 4)
    Check "frame generation's data files and subfolders too, not its logs" ((Test-Path (Join-Path $g1 'HelixSR/helixsr-fg/310.9.16/data.bin')) -and -not (Test-Path (Join-Path $g1 'HelixSR/helixsr-fg/nvexec.log')))
    Check 'says frame generation is included, off by default' ($r.out -match 'Frame generation included, off')
    Check 'no HelixSR-fsrfg folder' (-not (Test-Path (Join-Path $g1 'HelixSR-fsrfg')))
    if ($IsWindows -or $null -eq $IsWindows) {
        $t = [IO.File]::ReadAllText((Join-Path $g1 'OptiScaler.ini'))
        Check 'OptiScaler.ini: Dx12Upscaler=dlss, HelixSR=true, LoadAsiPlugins=true' ($t -match '(?m)^Dx12Upscaler=dlss\r?$' -and $t -match '(?m)^HelixSR=true\r?$' -and $t -match '(?m)^LoadAsiPlugins=true\r?$')
    }

    # the newest one wins: the HelixSR app's, made later
    Start-Sleep -Milliseconds 1100
    $app = Join-Path $w 'local/HelixSR-Desktop/releases/v3.1.0/HelixSR-3.1.0'
    Built (Join-Path $app 'HelixSR/helixsr.dll')
    $g2 = Join-Path $w 'g2'; Game $g2
    $r = Run $g2
    Check "finds the newest (the HelixSR app's)" ($r.code -eq 0 -and (Same (Join-Path $g2 'HelixSR/helixsr.dll') (Join-Path $app 'HelixSR/helixsr.dll')))
    Check "without helixsr-fg: upscaling only; without a built HLX FSR 4: AMD's FSR stays" (-not (Test-Path (Join-Path $g2 'HelixSR/helixsr-fg')) -and $r.out -match "isn't set up" -and (Get-Content -Raw (Join-Path $g2 'OptiScaler/amd_fidelityfx_upscaler_dx12.dll')) -match "AMD's FSR" -and -not (Test-Path (Join-Path $g2 'OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll')))

    # nothing found: a wrong file, then HelixSR's download folder typed in
    Remove-Item -Recurse -Force (Join-Path $w 'home'), (Join-Path $w 'local')
    $else = Join-Path $w 'else/HelixSR-3.1.0'
    Built (Join-Path $else 'HelixSR/helixsr.dll'); Fg (Join-Path $else 'HelixSR/helixsr-fg.off')
    Set-Content -LiteralPath (Join-Path $w 'else/not.dll') 'x'
    $g3 = Join-Path $w 'g3'; Game $g3
    $r = Run $g3 ((Join-Path $w 'else/not.dll') + "`n`"$else`"`n")
    Check 'refuses a wrong file' ($r.out -match "isn't HelixSR's")
    Check 'takes a typed download folder' ($r.code -eq 0 -and (Test-Path (Join-Path $g3 'HelixSR/helixsr-fg/nvexec64.dll')))

    $g4 = Join-Path $w 'g4'; Game $g4
    $r = Run $g4 "`n"
    Check 'nothing typed: stops, nothing copied' ($r.code -eq 1 -and -not (Test-Path (Join-Path $g4 'HelixSR')))

    # a reinstall with nothing else around: the game's own HelixSR stays, frame generation and HLX FSR 4 too
    $r = Run $g1
    Check "reinstall keeps the game's own" ($r.code -eq 0 -and (Get-ChildItem (Join-Path $g1 'HelixSR/helixsr-fg') -Filter *.dll).Count -eq 4 -and (Get-Content -Raw (Join-Path $g1 'OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll')) -match "AMD's FSR")

    # an unbuilt HLX FSR 4 put over AMD's by hand, no built one around: taken out, never kept as AMD's
    $g7 = Join-Path $w 'g7'; Game $g7
    Hlx (Join-Path $g7 'OptiScaler/amd_fidelityfx_upscaler_dx12.dll') $false
    Built (Join-Path $g7 'HelixSR/helixsr.dll')
    $r = Run $g7
    Check 'unbuilt HLX FSR 4 taken out' ($r.code -eq 0 -and -not (Test-Path (Join-Path $g7 'OptiScaler/amd_fidelityfx_upscaler_dx12.dll')) -and -not (Test-Path (Join-Path $g7 'OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll')))

    # an install of HelixSR 2.0 (HelixSR-fsrfg\): updated, that folder taken out
    $g5 = Join-Path $w 'g5'; Game $g5
    Set-Content -LiteralPath (New-Item -Force -ItemType File -Path (Join-Path $g5 'HelixSR-fsrfg/amd_fidelityfx_dx12.dll')) 'HelixSR 2.0'
    Built (Join-Path $g5 'HelixSR/helixsr.dll')
    $r = Run $g5
    Check 'HelixSR 2.0 install: HelixSR-fsrfg taken out' ($r.code -eq 0 -and (Test-Path (Join-Path $g5 'HelixSR/helixsr.dll')) -and -not (Test-Path (Join-Path $g5 'HelixSR-fsrfg')))

    # settings from before: [HelixSR] FrameGeneration=on becomes [FrameGen] FGInput=nvngxfg + FGNvngxReplacement=HelixSR
    # (Windows only: the \ to / rewrite above breaks the script's ini patterns elsewhere)
    if ($IsWindows -or $null -eq $IsWindows) {
        $g6 = Join-Path $w 'g6'
        Built (Join-Path $g6 'HelixSR/helixsr.dll'); Fg (Join-Path $g6 'HelixSR/helixsr-fg')
        $ini6 = Join-Path $g6 'OptiScaler.ini'
        [IO.File]::WriteAllText($ini6, "[HelixSR]`r`nFrameGeneration=on`r`nFGPresent=auto`r`nWaveSize=auto`r`n[FrameGen]`r`nFGInput=auto`r`nFGNvngxReplacement=auto`r`n")
        $r = Run $g6
        $t = [IO.File]::ReadAllText($ini6)
        Check 'old FrameGeneration=on: FGInput=nvngxfg, FGNvngxReplacement=HelixSR' ($r.code -eq 0 -and $t -match '(?m)^FGInput=nvngxfg\r?$' -and $t -match '(?m)^FGNvngxReplacement=HelixSR\r?$' -and $r.out -match 'included and on')
        Check 'old FrameGeneration / FGPresent lines removed, the rest kept' ($t -notmatch 'FrameGeneration=' -and $t -notmatch 'FGPresent=' -and $t -match '(?m)^WaveSize=auto\r?$')
    }
GNvngxReplacement=HelixSR
    # (Windows only: the \ to / rewrite above breaks the script's ini patterns elsewhere)
    if ($IsWindows -or $null -eq $IsWindows) {
        $g6 = Join-Path $w 'g6'
        Built (Join-Path $g6 'HelixSR-fsrfg/amd_fidelityfx_dx12.dll'); Fg (Join-Path $g6 'HelixSR-fsrfg/helixsr-fg')
        $ini6 = Join-Path $g6 'OptiScaler.ini'
        [IO.File]::WriteAllText($ini6, "[HelixSR]`r`nFrameGeneration=on`r`nFGPresent=auto`r`nWaveSize=auto`r`n[FrameGen]`r`nFGInput=auto`r`nFGNvngxReplacement=auto`r`n")
        $r = Run $g6
        $t = [IO.File]::ReadAllText($ini6)
        Check 'old FrameGeneration=on: FGInput=nvngxfg, FGNvngxReplacement=HelixSR' ($r.code -eq 0 -and $t -match '(?m)^FGInput=nvngxfg\r?$' -and $t -match '(?m)^FGNvngxReplacement=HelixSR\r?$' -and $r.out -match 'included and on')
        Check 'old FrameGeneration / FGPresent lines removed, the rest kept' ($t -notmatch 'FrameGeneration=' -and $t -notmatch 'FGPresent=' -and $t -match '(?m)^WaveSize=auto\r?$')
    }
} finally {
    Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
}

Write-Host ''
if ($fails -eq 0) { Write-Host 'all passed'; exit 0 } else { Write-Host "$fails failed"; exit 1 }
