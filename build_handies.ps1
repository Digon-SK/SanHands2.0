param(
    [string]$PluginSdk = 'C:\Users\Digon\Documents\Fuentes\plugin-sdk-master',
    [string]$RwFury = 'C:\Users\Digon\Documents\Fuentes\rwfury-master',
    [string]$PoseSource = 'C:\Users\Digon\Documents\ChatGPT\SanHands\dist\handpose.ifp',
    [string]$ModelsDir = 'C:\Users\Digon\Desktop\peds\con manos',
    [string]$InstallDir = 'C:\juegos\Grand Theft Auto San Andreas\modloader\Handies'
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$DistDir = Join-Path $ProjectDir 'dist'
$Compiler = 'C:\msys64\mingw32\bin\g++.exe'
$MingwBin = Split-Path -Parent $Compiler
$env:PATH = "$MingwBin;$env:PATH"

New-Item -ItemType Directory -Force -Path $DistDir, $InstallDir | Out-Null

$EmbeddedPose = Join-Path $DistDir 'Handies.ifp'
& python (Join-Path $ProjectDir 'tools\build_embedded_pose.py') `
    --source $PoseSource `
    --output $EmbeddedPose `
    --rwfury-root $RwFury
if ($LASTEXITCODE -ne 0) {
    throw "La generación de Handies.ifp falló con código $LASTEXITCODE"
}

$IncludeDirs = @(
    (Join-Path $PluginSdk 'plugin_sa'),
    (Join-Path $PluginSdk 'plugin_sa\game_sa'),
    (Join-Path $PluginSdk 'plugin_sa\game_sa\enums'),
    (Join-Path $PluginSdk 'plugin_sa\game_sa\rw'),
    (Join-Path $PluginSdk 'shared'),
    (Join-Path $PluginSdk 'shared\game'),
    (Join-Path $PluginSdk 'injector'),
    (Join-Path $PluginSdk 'hooking'),
    (Join-Path $PluginSdk 'safetyhook')
)

$OutputAsi = Join-Path $DistDir 'Handies.asi'
$Arguments = @(
    '-std=gnu++23', '-m32', '-O2', '-DNDEBUG', '-fpermissive',
    '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wshadow',
    '-DGTASA', '-DPLUGIN_SGV_10US', '-DRW', '-D_CRT_SECURE_NO_WARNINGS',
    '-DTARGET_NAME="Handies"',
    '-shared', (Join-Path $ProjectDir 'src\Handies.cpp'),
    '-o', $OutputAsi,
    ('-L' + (Join-Path $PluginSdk 'output\mingw\lib')),
    '-lPlugin', '-static-libgcc', '-static-libstdc++',
    '-Wl,--whole-archive', '-Wl,-Bstatic', '-lwinpthread',
    '-Wl,-Bdynamic', '-Wl,--no-whole-archive',
    '-Wl,--subsystem,windows', '-Wl,--exclude-all-symbols',
    '-Wl,--gc-sections', '-s'
)
foreach ($IncludeDir in $IncludeDirs) {
    $Arguments += '-isystem'
    $Arguments += $IncludeDir
}

& $Compiler @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "La compilación falló con código $LASTEXITCODE"
}

Copy-Item -Force -LiteralPath `
    $OutputAsi, `
    $EmbeddedPose, `
    (Join-Path $ProjectDir 'Handies.ini') `
    -Destination $InstallDir

$ModelFiles = Get-ChildItem -LiteralPath $ModelsDir -File |
    Where-Object { $_.Extension -in '.dff', '.txd' }
$DffCount = ($ModelFiles | Where-Object Extension -eq '.dff').Count
$TxdCount = ($ModelFiles | Where-Object Extension -eq '.txd').Count
if ($DffCount -ne 357 -or $TxdCount -ne 266) {
    throw "Lote incompleto en ${ModelsDir}: DFF=$DffCount TXD=$TxdCount"
}
$ModelFiles | Copy-Item -Destination $InstallDir -Force

Write-Host "Handies compilado e instalado en $InstallDir"
Write-Host "Modelos instalados: DFF=$DffCount TXD=$TxdCount"
