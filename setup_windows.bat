REM Setup OptiScaler for your game
@echo off
REM HelixSR Scaler: this file, for the HelixSR step at its end (PowerShell)
set "HSR_BAT=%~f0"
cls
echo  ::::::::  :::::::::  ::::::::::: :::::::::::  ::::::::   ::::::::      :::     :::        :::::::::: :::::::::  
echo :+:    :+: :+:    :+:     :+:         :+:     :+:    :+: :+:    :+:   :+: :+:   :+:        :+:        :+:    :+: 
echo +:+    +:+ +:+    +:+     +:+         +:+     +:+        +:+         +:+   +:+  +:+        +:+        +:+    +:+ 
echo +#+    +:+ +#++:++#+      +#+         +#+     +#++:++#++ +#+        +#++:++#++: +#+        +#++:++#   +#++:++#:  
echo +#+    +#+ +#+            +#+         +#+            +#+ +#+        +#+     +#+ +#+        +#+        +#+    +#+ 
echo #+#    #+# #+#            #+#         #+#     #+#    #+# #+#    #+# #+#     #+# #+#        #+#        #+#    #+# 
echo  ########  ###            ###     ###########  ########   ########  ###     ### ########## ########## ###    ### 
echo.
echo Coping is strong with this one...
echo v3.0-pre1
echo.

del "!! README_EXTRACT ALL FILES TO GAME FOLDER !!.txt" 2>nul

setlocal enabledelayedexpansion

if exist OptiScaler.sln (
    echo Detected OptiScaler.sln or .git files^^!
    echo.
    echo If .sln or .git files are in the folder, congratz, you have the source code.
	echo Now please try properly downloading OptiScaler.
	echo.
    echo Hint - use the Releases page on GitHub, or RTFM :^)
	echo.
    echo.
	echo P.S. If you somehow have both the OptiScaler.dll and .sln file, then be nice, just delete the .sln file, re-run the BAT setup and hope for the best.
	echo.
    goto end
)

if not exist OptiScaler.dll (
    echo OptiScaler "OptiScaler.dll" file is not found^^!
    echo Detected a folder permissions issue most likely. Might have more luck running the BAT as admin.
    echo.
	echo OR
	echo.
    echo If "OptiScaler.dll" exists, please manually rename to a supported filename ^(e.g. dxgi/winmm.dll^) and you are done^^!
	echo No need to run the setup BAT again after renaming.
	echo.
    echo.
    goto end
)

REM Check if old pre-0.9 additional files exist, along with an existing Opti installation
set "OLD_FILES_FOUND=0"
set "OPTI_DLL_LIST="
if exist nvapi64.dll set "OLD_FILES_FOUND=1"
if exist nvngx.dll set "OLD_FILES_FOUND=1"
if exist OptiScaler.asi set "OLD_FILES_FOUND=1"
if exist "Remove OptiScaler.bat" set "OLD_FILES_FOUND=1"
if exist "Remove_OptiScaler.bat" set "OLD_FILES_FOUND=1"

for %%F in (dxgi.dll winmm.dll d3d12.dll dbghelp.dll version.dll wininet.dll winhttp.dll) do (
    if exist "%%F" (
        set "origname="
        for /f "tokens=*" %%P in ('powershell -NoProfile -Command "(Get-Item '%%F').VersionInfo.OriginalFilename"') do (
            set "origname=%%P"
        )
        if /i "!origname!"=="OptiScaler.dll" (
            set "OLD_FILES_FOUND=1"
            set "OPTI_DLL_LIST=!OPTI_DLL_LIST! %%F"
        )
    )
)

if "!OLD_FILES_FOUND!"=="1" (
    echo WARNING: Possible old OptiScaler file^(s^) detected^^!
    if exist nvapi64.dll echo   - nvapi64.dll
    if exist nvngx.dll echo   - nvngx.dll
    if exist OptiScaler.asi echo   - OptiScaler.asi
	if exist "Remove OptiScaler.bat" echo   - Remove OptiScaler.bat
    if exist "Remove_OptiScaler.bat" echo   - Remove_OptiScaler.bat
    for %%F in (!OPTI_DLL_LIST!) do echo   - %%F ^(original filename: OptiScaler.dll^)
    echo.
    echo These files may conflict with the current version of OptiScaler.
    echo It is recommended to delete them.
    echo.
    echo Do you want to delete these files?
    echo.
	echo [1] Yes
    echo [2] No
    echo.
	set /p "USER_CHOICE=Waiting - "
	echo.
    if /i "!USER_CHOICE!"=="1" (
        if exist nvapi64.dll (
            del nvapi64.dll
            echo Deleted nvapi64.dll
        )
        if exist nvngx.dll (
            del nvngx.dll
            echo Deleted nvngx.dll
        )
        if exist OptiScaler.asi (
            del OptiScaler.asi
            echo Deleted OptiScaler.asi
        )
		if exist "Remove OptiScaler.bat" (
            del "Remove OptiScaler.bat"
            echo Deleted Remove OptiScaler.bat
        )
        if exist "Remove_OptiScaler.bat" (
            del "Remove_OptiScaler.bat"
            echo Deleted Remove_OptiScaler.bat
        )
        for %%F in (!OPTI_DLL_LIST!) do (
            del "%%F"
            echo Deleted %%F
        )
        echo Done^^!
    ) else (
        echo Skipping deletion. Note that these files may cause issues.
    )
    echo.
)

REM Set paths based on current directory

set "optiScalerFile=.\OptiScaler.dll"
set setupSuccess=false

REM Check if the Engine folder exists
if exist ".\Engine" (
    echo Found Engine folder. If this is an Unreal Engine game, then please extract Optiscaler to #CODENAME#\Binaries\Win64
	echo Do not extract to the Engine folder^^!
	echo.
	echo Example - \Jedi Survivor\SwGame\Binaries\Win64, \Witchfire\Witchfire\Binaries\Win64
    echo.
    echo Continue installation to current folder?
	echo. 
    echo [1] Yes
    echo [2] No
    echo.
	set /p continueChoice="Waiting - "
    set continueChoice=!continueChoice: =!

    if "!continueChoice!"=="1" (
        goto selectFilename
    )

    goto end
)

REM Prompt user to select a filename for OptiScaler
:selectFilename
echo.
echo Choose a filename for OptiScaler (default is dxgi.dll, most compatible):
echo (For Vulkan, use winmm.dll. For XGP/MS Store, winmm/version.dll may be better)
echo.
echo  [1] dxgi.dll
echo  [2] winmm.dll
echo  [3] version.dll
echo  [4] dbghelp.dll
echo  [5] d3d12.dll
echo  [6] wininet.dll
echo  [7] winhttp.dll
echo  [8] OptiScaler.asi
echo.
set /p filenameChoice="Enter 1-8 (or press Enter for default): "

if "%filenameChoice%"=="" (
    set selectedFilename="dxgi.dll"
) else if "%filenameChoice%"=="1" (
    set selectedFilename="dxgi.dll"
) else if "%filenameChoice%"=="2" (
    set selectedFilename="winmm.dll"
) else if "%filenameChoice%"=="3" (
    set selectedFilename="version.dll"
) else if "%filenameChoice%"=="4" (
    set selectedFilename="dbghelp.dll"
) else if "%filenameChoice%"=="5" (
    set selectedFilename="d3d12.dll"
) else if "%filenameChoice%"=="6" (
    set selectedFilename="wininet.dll"
) else if "%filenameChoice%"=="7" (
    set selectedFilename="winhttp.dll"
) else if "%filenameChoice%"=="8" (
    set selectedFilename="OptiScaler.asi"
) else (
    echo Invalid choice. Please select a valid option.
    echo.
    goto selectFilename
)

if exist %selectedFilename% (
    echo.
    echo WARNING: %selectedFilename% already exists in the current folder.
    echo.
	echo Do you want to overwrite %selectedFilename%?
	echo.
    echo [1] Yes
    echo [2] No
    echo.
	set /p overwriteChoice="Waiting - "
    set overwriteChoice=!overwriteChoice: =!
    
    echo.
    if "!overwriteChoice!"=="1" (
        goto checkWine
    )

    goto selectFilename
)

REM Wine doesn't support powershell
:checkWine
reg query HKEY_CURRENT_USER\Software\Wine\DllOverrides >nul 2>&1
if %errorlevel%==0 (
    echo.
    echo Using wine, skipping over spoofing checks.
    echo If you need, you can disable spoofing by setting Dxgi=false in the config
    echo.
    pause
    goto completeSetup
) 

if exist %windir%\system32\nvapi64.dll (
    echo.
    echo Nvidia driver files detected.
    set isNvidia=true
) else (
    set isNvidia=false
)

REM Query user for GPU type
echo.
echo Are you using an Nvidia GPU or AMD/Intel GPU?
echo.
echo [1] AMD/Intel
echo [2] Nvidia
echo.

:gpuPrompt
if "%isNvidia%"=="true" (
    set /p gpuChoice="Enter 1 or 2 (Detected Nvidia): "
) else (
    set /p gpuChoice="Enter 1 or 2 (Detected AMD/Intel): "
)

if "%gpuChoice%"=="1" goto gpuValid
if "%gpuChoice%"=="2" goto gpuValid
echo Invalid input. Please enter 1 or 2.
echo.
goto gpuPrompt

:gpuValid

REM Skip spoofing if Nvidia
if "%gpuChoice%"=="2" (
    goto completeSetup
)

REM Query user for DLSS
echo.
echo Will you try to use DLSS inputs to replace with FSR/XeSS? (enables Nvidia spoofing, required for DLSS-FG, Reflex-^>AL2)
echo If you want to change the setting later, edit OptiScaler.ini and set Dxgi=false to disable spoofing and reverse.
echo.
echo [1] Yes
echo [2] No
echo.
set /p enablingSpoofing="Enter 1 or 2 (or press Enter for Yes): "

set configFile=OptiScaler.ini
if "%enablingSpoofing%"=="2" (
    if not exist "%configFile%" (
        echo Config file not found: %configFile%
        pause
    )

    powershell -Command "(Get-Content '%configFile%') -replace 'Dxgi=auto', 'Dxgi=false' | Set-Content '%configFile%'"
)

REM Decide whether to run OptiPatcher
echo.
if "%gpuChoice%"=="1" (
    echo AMD/Intel GPU detected - running OptiPatcher check.
    goto checkExistingOptiPatcher
)

:checkExistingOptiPatcher
set "foundOptiPatcher="
for %%F in (OptiScaler\plugins\*OptiPatcher*.asi) do (
    set "foundOptiPatcher=%%F"
)

if defined foundOptiPatcher (
    echo.
    echo OptiPatcher found: !foundOptiPatcher!
    echo If the existing version works properly, might be best to keep it.
	echo Do you want to re-download a possibly newer version?
	echo.
    echo [1] Yes
    echo [2] No
    echo.
	set /p optiRedownload="Waiting - "
        
    if /i "!optiRedownload!"=="1" (
        echo.
        echo Deleting !foundOptiPatcher!...
        del "!foundOptiPatcher!"
        goto checkOptiPatcher
    ) else (
        echo.
        echo Keeping existing OptiPatcher - skipping download.
        goto completeSetup
    )
)

REM Not installed - continue to download
goto checkOptiPatcher

:checkOptiPatcher
REM Check connectivity
echo.
echo Checking for OptiPatcher compatibility...
echo Press Ctrl+C if this gets stuck to skip to setup completion.

ping -n 1 -w 3000 github.com >nul 2>&1
if %errorlevel% neq 0 (
    echo Offline or GitHub blocked. Skipping OptiPatcher check.
    goto completeSetup
)

set "OPTI_MATCH=NO"
for /f "usebackq tokens=*" %%A in (`powershell -Command "& { $rawUrl = 'https://raw.githubusercontent.com/optiscaler/OptiPatcher/main/OptiPatcher/dllmain.cpp'; try { $code = (Invoke-WebRequest -Uri $rawUrl -UseBasicParsing).Content } catch { return 'ERR' }; $supported = @(); $ueMatches = [Regex]::Matches($code, 'CHECK_UE\s*\(\s*([a-zA-Z0-9_]+)\s*\)'); foreach ($m in $ueMatches) { $base = $m.Groups[1].Value; $supported += ($base + '-win64-shipping.exe').ToLower(); $supported += ($base + '-wingdk-shipping.exe').ToLower(); }; $directMatches = [Regex]::Matches($code, 'exeName\s*==\s*[\x22\x27]([^\x22\x27]+)[\x22\x27]'); foreach ($m in $directMatches) { $supported += $m.Groups[1].Value.ToLower(); }; $localFiles = Get-ChildItem *.exe | Select-Object -ExpandProperty Name; foreach ($file in $localFiles) { if ($supported -contains $file.ToLower()) { Write-Output 'YES'; exit; } }; Write-Output 'NO'; }"`) do (
    set "OPTI_MATCH=%%A"
)

if "!OPTI_MATCH!"=="YES" (
    echo.
    echo OptiPatcher support detected^^!
    echo An Opti plugin used for unlocking DLSS/DLSS-FG inputs, avoiding spoofing and performance overhead in supported games.
    echo More info available on OptiPatcher Github
    echo.
	echo Download OptiPatcher.asi?
    echo.
	echo [1] Yes
    echo [2] No
    echo.
	set /p downloadOptiPatcher="Waiting - "
    set downloadOptiPatcher=!downloadOptiPatcher: =!
    
    if "!downloadOptiPatcher!"=="1" (
        echo.
        echo Preparing plugins folder...
        if not exist "OptiScaler\plugins" mkdir "OptiScaler\plugins"
        
        echo Downloading OptiPatcher...
        echo Press Ctrl+C if this gets stuck to skip to setup completion.
        echo.
        powershell -Command "Invoke-WebRequest -Uri 'https://github.com/optiscaler/OptiPatcher/releases/download/rolling/OptiPatcher.asi' -OutFile 'OptiScaler\plugins\OptiPatcher.asi'"
        if errorlevel 1 goto completeSetup
        
        if exist "OptiScaler\plugins\OptiPatcher.asi" (
            echo OptiPatcher.asi downloaded successfully.
            echo Enabling ASI loading in OptiScaler.ini...
            if exist "%configFile%" (
                powershell -Command "(Get-Content '%configFile%') -replace 'LoadAsiPlugins=auto', 'LoadAsiPlugins=true' | Set-Content '%configFile%'"
                echo Successfully enabled ASI loading in OptiScaler.ini^^!
            ) else (
                echo Warning: OptiScaler.ini not found, could not enable LoadAsiPlugins.
            )
        ) else (
            echo Failed to download OptiPatcher.asi.
        )
     timeout /t 3
    )
)
echo.

goto completeSetup

:completeSetup
REM HelixSR Scaler: HLX DLSS into HelixSR\helixsr.dll (frame generation into HelixSR\helixsr-fg), HLX FSR 4 into OptiScaler\
call :helixsr
if errorlevel 1 goto end

REM Rename OptiScaler file
echo.
if "!overwriteChoice!"=="1" (
    echo Removing previous %selectedFilename%...
    del /F %selectedFilename% 
)

echo Renaming OptiScaler file to %selectedFilename%...
rename "%optiScalerFile%" %selectedFilename%
if errorlevel 1 (
    echo.
    echo ERROR: Failed to rename OptiScaler file to %selectedFilename%. Most likely due to folder permissions issues.
    echo Please rename OptiScaler.dll manually to %selectedFilename%^^! No need to run setup BAT again after that.
    echo.
    goto end
)

goto create_uninstaller

:create_uninstaller_return

cls
echo  OptiScaler setup completed successfully...
echo.
echo   ___                 
echo  (_         '        
echo  /__  /)   /  () (/  
echo          _/      /    
echo.

set setupSuccess=true

:end
pause

if "%setupSuccess%"=="true" (
    del "setup_linux.sh"
    del "%~nx0"
)

exit /b

:create_uninstaller
setlocal DisableDelayedExpansion

(
echo @echo off
echo setlocal EnableDelayedExpansion
echo cls
echo echo  ::::::::  :::::::::  ::::::::::: :::::::::::  ::::::::   ::::::::      :::     :::        :::::::::: :::::::::  
echo echo :+:    :+: :+:    :+:     :+:         :+:     :+:    :+: :+:    :+:   :+: :+:   :+:        :+:        :+:    :+: 
echo echo +:+    +:+ +:+    +:+     +:+         +:+     +:+        +:+         +:+   +:+  +:+        +:+        +:+    +:+ 
echo echo +#+    +:+ +#++:++#+      +#+         +#+     +#++:++#++ +#+        +#++:++#++: +#+        +#++:++#   +#++:++#:  
echo echo +#+    +#+ +#+            +#+         +#+            +#+ +#+        +#+     +#+ +#+        +#+        +#+    +#+ 
echo echo #+#    #+# #+#            #+#         #+#     #+#    #+# #+#    #+# #+#     #+# #+#        #+#        #+#    #+# 
echo echo  ########  ###            ###     ###########  ########   ########  ###     ### ########## ########## ###    ### 
echo echo.
echo echo Coping is strong with this one...
echo echo v2.8 - now with OptiPatcher support
echo echo.
echo REM Check if OptiScaler installation exists
echo set "OLD_FILES_FOUND=0"
echo set "OPTI_DLL_LIST="
echo if exist OptiScaler.asi set "OLD_FILES_FOUND=1"

echo for %%%%F in ^(dxgi.dll winmm.dll d3d12.dll dbghelp.dll version.dll wininet.dll winhttp.dll^) do ^(
echo     if exist "%%%%F" ^(
echo         set "origname="
echo         for /f "tokens=*" %%%%P in ^('powershell -NoProfile -Command "(Get-Item '%%%%F').VersionInfo.OriginalFilename"'^) do ^(
echo             set "origname=%%%%P"
echo         ^)
echo         if /i "!origname!"=="OptiScaler.dll" ^(
echo             set "OLD_FILES_FOUND=1"
echo             set "OPTI_DLL_LIST=!OPTI_DLL_LIST! %%%%F"
echo         ^)
echo     ^)
echo ^)

echo if "!OLD_FILES_FOUND!"=="1" ^(
echo     echo Existing OptiScaler installation detected^^^^!
echo     if exist OptiScaler.asi echo   - OptiScaler.asi
echo     for %%%%F in ^(!OPTI_DLL_LIST!^) do echo   - %%%%F - original filename: OptiScaler.dll
echo     echo.
echo ^)

echo echo Do you want to remove OptiScaler?
echo echo.
echo echo [1] Yes
echo echo [2] No
echo echo.
echo set /p removeChoice="Waiting - "
echo echo.

echo if "%%removeChoice%%"=="1" ^(
echo     del OptiScaler.log
echo     del OptiScaler.ini
echo     del OptiScaler.asi
echo     for %%%%F in ^(!OPTI_DLL_LIST!^) do ^(del "%%%%F"^)
echo     del /Q Licenses\*
echo     rd Licenses
echo     del /Q OptiScaler\D3D12_Optiscaler\*
echo     rd OptiScaler\D3D12_Optiscaler
echo     del /Q OptiScaler\Streamline\*
echo     rd OptiScaler\Streamline
echo     del /Q OptiScaler\streamline\*
echo     rd OptiScaler\streamline
echo     echo.
echo     echo Deleting OptiPatcher if present
echo     del /Q OptiScaler\plugins\*
echo     rd OptiScaler\plugins
echo     echo.
echo     del /Q OptiScaler\*
echo     rd OptiScaler
echo     echo Deleting HelixSR
echo     rd /s /q HelixSR-fsrfg
echo     rd /s /q HelixSR
echo     echo.
echo     echo OptiScaler removed^^^^! Ignore the warnings about missing files.
echo     echo.
echo ^) else ^(
echo     echo.
echo     echo Operation cancelled.
echo     echo.
echo ^)

echo.
echo pause
echo if "%%removeChoice%%"=="1" ^(
echo     del "%%~nx0"
echo ^)
) > "Remove_OptiScaler.bat"

endlocal
echo.
echo Uninstaller created.
echo.

goto create_uninstaller_return

REM HelixSR Scaler: finds HelixSR 3.x after its setup built it (the HelixSR download, the HelixSR app, or a path typed
REM in) and copies HLX DLSS to HelixSR\helixsr.dll (with frame generation when HelixSR's setup made it) and HLX FSR 4 to
REM OptiScaler\amd_fidelityfx_upscaler_dx12.dll, AMD's FSR next to it as amd_fidelityfx_upscaler_dx12.amd.dll.
REM The PowerShell part is at the end of this file, after #HSR-PS-START.
:helixsr
where powershell >nul 2>&1
if errorlevel 1 (
    echo.
    echo PowerShell isn't available ^(Wine?^): copy HelixSR by hand, see "Install by hand" in HelixSR's README.
    echo On Linux, use setup_linux.sh instead.
    echo.
    exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:HSR_BAT); Invoke-Expression $s.Substring($s.LastIndexOf('#HSR' + '-PS-START'))"
exit /b %errorlevel%

#HSR-PS-START
$ErrorActionPreference = 'Stop'
$game = (Get-Location).Path
$fgFiles = 'nvexec64.dll', '_nvngx.dll', 'nvngx_dlssg.dll'
$ffx = Join-Path $game 'OptiScaler\amd_fidelityfx_upscaler_dx12.dll'
$ffxAmd = Join-Path $game 'OptiScaler\amd_fidelityfx_upscaler_dx12.amd.dll'

# the last bytes of a file as text
function Get-Tail([string]$f, [int]$from, [int]$n) {
    if (-not $f -or -not (Test-Path -LiteralPath $f -PathType Leaf)) { return '' }
    $fs = $null
    try {
        $fs = [IO.File]::OpenRead($f)
        if ($fs.Length -lt $from) { return '' }
        [void]$fs.Seek(-$from, [IO.SeekOrigin]::End)
        $b = New-Object byte[] $n
        [void]$fs.Read($b, 0, $n)
        return [Text.Encoding]::ASCII.GetString($b)
    } catch { return '' } finally { if ($fs) { $fs.Dispose() } }
}
# HelixSR's DLL with the network built into it ends with a 32-byte trailer that starts with HXSRNET1 (HXSRFG01 when
# frame generation's data came after it)
function Test-Built([string]$f) { (Get-Tail $f 32 8) -in 'HXSRNET1', 'HXSRFG01' }
# HLX FSR 4 with its shaders built in (by HelixSR's setup) ends with a 24-byte trailer that starts with HLXFSR4D
function Test-Hlx([string]$f) { (Get-Tail $f 24 8) -eq 'HLXFSR4D' }
# an HLX FSR 4 DLL, built or not (its DLLs carry its name)
function Test-HlxAny([string]$f) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { return $false }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)).Contains('HLX FSR 4')
}

# HelixSR's built HelixSR\helixsr.dll: the newest in the usual places
function Find-HelixSR {
    $home_ = $env:USERPROFILE
    $places = @(
        (Join-Path $env:LOCALAPPDATA 'HelixSR-Desktop\releases\*\HelixSR\helixsr.dll'),   # the HelixSR app
        (Join-Path $env:LOCALAPPDATA 'HelixSR-Desktop\releases\*\HelixSR*\HelixSR\helixsr.dll'),
        (Join-Path $home_ 'Downloads\HelixSR*\HelixSR\helixsr.dll'),
        (Join-Path $home_ 'Downloads\*\HelixSR*\HelixSR\helixsr.dll'),
        (Join-Path $home_ 'Desktop\HelixSR*\HelixSR\helixsr.dll'),
        (Join-Path $home_ 'Documents\HelixSR*\HelixSR\helixsr.dll'),
        (Join-Path $home_ 'HelixSR*\HelixSR\helixsr.dll'),
        'C:\HelixSR*\HelixSR\helixsr.dll'
    )
    $hit = $places | ForEach-Object { Get-ChildItem -Path $_ -File -ErrorAction SilentlyContinue } |
        Where-Object { Test-Built $_.FullName } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    # a reinstall: the HelixSR already in the game
    $own = Join-Path $game 'HelixSR\helixsr.dll'
    if (Test-Built $own) { return $own }
    return $null
}

Write-Host ''
Write-Host 'HelixSR Scaler: looking for HelixSR...'
$helix = Find-HelixSR
while (-not $helix) {
    Write-Host ''
    Write-Host "HelixSR wasn't found. Download HelixSR 3.0 or newer and run its helixsr-setup first (it builds HelixSR on"
    Write-Host 'your PC), or set it up with the HelixSR app. Then drag the HelixSR download folder (or its HelixSR\helixsr.dll) in here.'
    Write-Host 'Press Enter on its own to stop.'
    $in = (Read-Host 'HelixSR').Trim().Trim('"').Trim("'")
    if (-not $in) { Write-Host 'Nothing installed.'; [Environment]::Exit(1) }
    if (Test-Path -LiteralPath $in -PathType Container) {
        $in = @((Join-Path $in 'HelixSR\helixsr.dll'), (Join-Path $in 'helixsr.dll')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if ($in -and (Test-Built $in)) { $helix = (Resolve-Path -LiteralPath $in).Path }
    else { Write-Host "That isn't HelixSR's HelixSR\helixsr.dll after its setup (run helixsr-setup first)." }
}
Write-Host "HLX DLSS: $helix"
$src = Split-Path -Parent $helix
$root = Split-Path -Parent $src

# HLX FSR 4: the one in this folder when it is built, else the one HelixSR's setup built in that HelixSR download
$hlx = $null
foreach ($c in $ffx, (Join-Path $root 'HLX-FSR4\amd_fidelityfx_upscaler_dx12.dll'), (Join-Path $root 'helixsr-scaler\OptiScaler\amd_fidelityfx_upscaler_dx12.dll')) {
    if (Test-Hlx $c) { $hlx = $c; break }
}

# Frame generation: the helixsr-fg folder HelixSR's setup made next to its DLL (a reinstall: the game's own copy)
$fg = $null
foreach ($d in (Join-Path $src 'helixsr-fg'), (Join-Path $src 'helixsr-fg.off')) {
    if (@($fgFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $d $_) -PathType Leaf) }).Count -eq 0) { $fg = $d; break }
}

try {
    $dst = Join-Path $game 'HelixSR'
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('helixsr-' + [Guid]::NewGuid())
    New-Item -ItemType Directory -Force -Path (Join-Path $tmp 'fg') | Out-Null
    # copied out first: a reinstall's source is the game's own HelixSR folder
    Copy-Item -LiteralPath $helix -Destination (Join-Path $tmp 'helixsr.dll') -Force
    $fgDll = Join-Path $src 'helixsr_fg.dll'
    $hasFgDll = Test-Path -LiteralPath $fgDll -PathType Leaf
    if ($hasFgDll) { Copy-Item -LiteralPath $fgDll -Destination (Join-Path $tmp 'helixsr_fg.dll') -Force }
    if ($hlx) { Copy-Item -LiteralPath $hlx -Destination (Join-Path $tmp 'hlx.dll') -Force }
    # frame generation: all of its files (data files and subfolders too), not its logs
    if ($fg) {
        Get-ChildItem -LiteralPath $fg -Recurse -File | Where-Object { $_.Name -notlike '*.log' -and $_.Name -notlike '*.log.*' } |
            ForEach-Object {
                $rel = $_.FullName.Substring($fg.Length).TrimStart('\', '/')
                $to = Join-Path (Join-Path $tmp 'fg') $rel
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $to -Force
            }
    }
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -LiteralPath (Join-Path $tmp 'helixsr.dll') -Destination (Join-Path $dst 'helixsr.dll') -Force
    if ($hasFgDll) { Copy-Item -LiteralPath (Join-Path $tmp 'helixsr_fg.dll') -Destination (Join-Path $dst 'helixsr_fg.dll') -Force }
    if ($fg) {
        foreach ($old in 'helixsr-fg', 'helixsr-fg.off') {
            $o = Join-Path $dst $old
            if (Test-Path -LiteralPath $o) { Remove-Item -LiteralPath $o -Recurse -Force }
        }
        Copy-Item -LiteralPath (Join-Path $tmp 'fg') -Destination (Join-Path $dst 'helixsr-fg') -Recurse -Force
    }
    # HLX FSR 4 under the FidelityFX name, AMD's FSR next to it as .amd.dll (HLX FSR 4 hands AMD's versions to it)
    if ($hlx) {
        if (-not (Test-Path -LiteralPath $ffxAmd) -and (Test-Path -LiteralPath $ffx) -and -not (Test-HlxAny $ffx)) {
            Move-Item -LiteralPath $ffx -Destination $ffxAmd -Force
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ffx) | Out-Null
        Copy-Item -LiteralPath (Join-Path $tmp 'hlx.dll') -Destination $ffx -Force
        Write-Host "HLX FSR 4: $hlx"
    } else {
        # not built: an HLX FSR 4 without its shaders has nothing to run, AMD's FSR goes back under its own name
        if (Test-HlxAny $ffx) {
            if (Test-Path -LiteralPath $ffxAmd) { Move-Item -LiteralPath $ffxAmd -Destination $ffx -Force }
            else { Remove-Item -LiteralPath $ffx -Force }
        }
        Write-Host "HLX FSR 4 isn't built (run HelixSR's setup): AMD's FSR stays."
    }
    # HelixSR 2.0's folder (HelixSR Scaler 1.1.0): HelixSR Scaler would still use it, so it goes
    $legacy = Join-Path $game 'HelixSR-fsrfg'
    if (Test-Path -LiteralPath $legacy) {
        Remove-Item -LiteralPath $legacy -Recurse -Force
        Write-Host 'The HelixSR-fsrfg folder of an older install was taken out (HelixSR is in HelixSR\ now)'
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
} catch {
    Write-Host "Could not copy HelixSR into the game: $($_.Exception.Message)"
    Write-Host 'Most likely a folder permissions issue: try running this setup as admin.'
    [Environment]::Exit(1)
}
Write-Host 'HLX DLSS copied to HelixSR\helixsr.dll'

# OptiScaler.ini: what HelixSR needs where it's left on auto (as HelixSR's setup and the HelixSR app write it):
# [Upscalers] Dx12Upscaler=dlss (HLX DLSS is OptiScaler's DLSS upscaler), [DLSS] HelixSR=true, [Plugins]
# LoadAsiPlugins=true (OptiPatcher)
$ini = Join-Path $game 'OptiScaler.ini'
if (Test-Path -LiteralPath $ini -PathType Leaf) {
    $lines = [IO.File]::ReadAllText($ini) -split '\n'
    $want = @{ 'Upscalers|Dx12Upscaler' = @('dlss', 'auto', 'ffx'); 'DLSS|HelixSR' = @('true', 'auto'); 'Plugins|LoadAsiPlugins' = @('true', 'auto') }
    $sec = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i].TrimEnd("`r")
        if ($l -match '^\[(.+)\]') { $sec = $Matches[1]; continue }
        if ($l -match '^([A-Za-z0-9]+)[ \t]*=[ \t]*(.*)$') {
            $w = $want["$sec|$($Matches[1])"]
            if ($w -and ($w[1..($w.Count - 1)] -contains $Matches[2].Trim().ToLower())) {
                $lines[$i] = "$($Matches[1])=$($w[0])" + $(if ($lines[$i].EndsWith("`r")) { "`r" } else { '' })
            }
        }
    }
    [IO.File]::WriteAllText($ini, ($lines -join "`n"))
}

if ($fg) {
    # Frame generation: HelixSR is OptiScaler's DLSS frame-generation provider, on with [FrameGen] FGInput=nvngxfg and
    # FGNvngxReplacement=HelixSR in OptiScaler.ini (asked when OptiScaler.ini leaves frame generation on auto)
    $on = $false
    if (Test-Path -LiteralPath $ini -PathType Leaf) {
        $text = [IO.File]::ReadAllText($ini)
        $switchOn = {
            param($t)
            $t = $t -replace '(?m)^FGInput[ \t]*=[^\r\n]*', 'FGInput=nvngxfg'
            $t -replace '(?m)^FGNvngxReplacement[ \t]*=[^\r\n]*', 'FGNvngxReplacement=HelixSR'
        }
        $inputAuto = $text -match '(?m)^FGInput[ \t]*=[ \t]*auto[ \t]*\r?$'
        # settings from before: [HelixSR] FrameGeneration=on is now the two [FrameGen] lines
        if ($inputAuto -and $text -match '(?m)^FrameGeneration[ \t]*=[ \t]*(on|true|1)[ \t]*\r?$') {
            $text = & $switchOn $text
            $inputAuto = $false
        }
        $text = $text -replace '(?m)^(FrameGeneration|FGPresent)[ \t]*=[^\r\n]*\r?\n', ''
        if ($text -match '(?m)^FGInput[ \t]*=[ \t]*nvngxfg[ \t]*\r?$' -and $text -match '(?m)^FGNvngxReplacement[ \t]*=[ \t]*HelixSR[ \t]*\r?$') { $on = $true }
        elseif ($inputAuto -and -not [Console]::IsInputRedirected) {
            $a = (Read-Host 'Turn on HelixSR frame generation in this game (DirectX 12 games with DLSS Frame Generation)? [Y/n]').Trim()
            if ($a -eq '' -or $a -match '^(y|yes)$') {
                $text = & $switchOn $text
                $on = $true
            }
        }
        [IO.File]::WriteAllText($ini, $text)
    }
    if ($on) { Write-Host 'Frame generation included and on (HelixSR\helixsr-fg). In the game, turn on its frame generation.' }
    else { Write-Host 'Frame generation included, off: turn it on in the menu (Insert, Frame Generation: HelixSR) or with FGInput=nvngxfg and FGNvngxReplacement=HelixSR in OptiScaler.ini.' }
}
else { Write-Host "Frame generation isn't set up: upscaling only." }
Write-Host 'In the game, pick DLSS: that runs HLX DLSS. Insert opens the menu; its upscaler list also has HLX FSR 4.'
[Environment]::Exit(0)
