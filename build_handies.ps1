param(
    [string]$PluginSdk = 'C:\Users\Digon\Documents\Fuentes\plugin-sdk-master',
    [string]$RwFury = 'C:\Users\Digon\Documents\Fuentes\rwfury-master',
    [string]$DragonFF = 'C:\Users\Digon\AppData\Roaming\Blender Foundation\Blender\5.2\extensions\user_default\dragonff',
    [string]$PoseSource = 'C:\Users\Digon\Documents\ChatGPT\SanHands\dist\handpose.ifp',
    [string]$GangAnimationSource = 'C:\juegos\Grand Theft Auto San Andreas\modloader\hands\ghands.ifp',
    [string]$SourceModels = 'C:\Users\Digon\Desktop\peds',
    [string]$Hands = 'C:\Users\Digon\Desktop\hands',
    [string]$ModelsDir = 'C:\Users\Digon\Desktop\peds\con manos',
    [string]$InstallDir = 'C:\juegos\Grand Theft Auto San Andreas\modloader\Handies'
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$DistDir = Join-Path $ProjectDir 'dist'
$ExpandedDir = Join-Path $DistDir 'expanded-models'
$Compiler = 'C:\msys64\mingw32\bin\g++.exe'
$env:PATH = "$(Split-Path -Parent $Compiler);$env:PATH"
$env:DRAGONFF_PATH = $DragonFF

New-Item -ItemType Directory -Force -Path `
    $DistDir, $ExpandedDir, $ModelsDir, $InstallDir | Out-Null

# First build the already stitched/UV-mapped geometry with its temporary
# 62-bone authoring rig. The finalizer immediately removes that rig from DFFs.
& python (Join-Path $ProjectDir 'add_hands.py') `
    --input $SourceModels `
    --hands $Hands `
    --output $ExpandedDir `
    --dragonff $DragonFF `
    --copy-txd
if ($LASTEXITCODE -ne 0) {
    throw "La integración geométrica de las manos falló con código $LASTEXITCODE"
}

$RuntimeData = Join-Path $DistDir 'Handies.dat'
& python (Join-Path $ProjectDir 'tools\finalize_runtime_models.py') `
    --source $SourceModels `
    --expanded $ExpandedDir `
    --output $ModelsDir `
    --data $RuntimeData `
    --pose-source $PoseSource `
    --gang-source $GangAnimationSource `
    --dragonff $DragonFF `
    --rwfury-root $RwFury
if ($LASTEXITCODE -ne 0) {
    throw "La restauración del esqueleto nativo falló con código $LASTEXITCODE"
}

& python (Join-Path $ProjectDir 'validate_outputs.py') `
    --source $SourceModels `
    --output $ModelsDir `
    --hands $Hands `
    --dragonff $DragonFF `
    --runtime-data $RuntimeData
if ($LASTEXITCODE -ne 0) {
    throw "La validación del lote falló con código $LASTEXITCODE"
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
    $RuntimeData, `
    (Join-Path $ProjectDir 'Handies.ini') `
    -Destination $InstallDir

# Versiones anteriores instalaban un IFP parcial. Ya no se carga; se conserva
# una copia desactivada y recuperable para evitar confundirlo con una
# dependencia de esta versión.
$OldIfp = Join-Path $InstallDir 'Handies.ifp'
if (Test-Path -LiteralPath $OldIfp -PathType Leaf) {
    Move-Item -LiteralPath $OldIfp -Destination "$OldIfp.obsolete" -Force
}

$ModelFiles = Get-ChildItem -LiteralPath $ModelsDir -File |
    Where-Object { $_.Extension -in '.dff', '.txd' }
$DffCount = ($ModelFiles | Where-Object Extension -eq '.dff').Count
$TxdCount = ($ModelFiles | Where-Object Extension -eq '.txd').Count
if ($DffCount -ne 357 -or $TxdCount -ne 266) {
    throw "Lote incompleto en ${ModelsDir}: DFF=$DffCount TXD=$TxdCount"
}
$ModelFiles | Copy-Item -Destination $InstallDir -Force

Write-Host "Handies compilado e instalado en $InstallDir"
Write-Host "DFF nativos con geometría nueva: $DffCount; TXD: $TxdCount"
Write-Host "Los huesos GHANDS generan perfiles morph; el juego conserva sólo sus 32 huesos"
