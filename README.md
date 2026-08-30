# SanHands 2.0

SanHands 2.0 adds articulated, animated fingers to GTA San Andreas pedestrians while preserving the game's native ped animation system.

The generated DFFs contain the replacement hand geometry, closed wrist seams, corrected UV islands, and **only the original 32-bone GTA ped skeleton**. Finger bones are used only while building per-hand morph profiles; `Handies.asi` never inserts them into GTA's HAnim hierarchy.

## Design

- Replaces the original closed hand geometry at the wrist.
- Joins the hand and forearm with bridge triangles, without gaps or overlapping open edges.
- Preserves the detailed hand UV topology and maps it into the ped's dominant skin region.
- Restores the original 32-node HAnim hierarchy and 32-bone Skin PLG before writing each final DFF.
- Keeps GTA's normal walk, fight, weapon, gesture, and body animation associations at 32 frames.
- Builds a geometry-specific hand profile containing its finger vertex mask, gang-hand weights, bind matrices, offsets, and base normals.
- Imports every animation in `ghands.ifp` that contains the complete 15-track finger rig as a named hand sequence.
- Selects those sequences through configurable hand profiles mapped to live ped animation associations.
- Applies the selected morph to the shared hand vertices immediately before `RpClumpRender`, renders through the untouched native 32-bone Skin, and restores the base vertices immediately afterward.
- Never creates a 62-node runtime hierarchy, swaps HAnim owners, or changes `CAnimBlendClumpData`, `finger`, or `finger01`.
- Uses the active ped association's real group, animation ID, playback time, blend amount, and configurable priority, while suppressing only the native separate replacement-hand render pass.
- Copies TXD files byte-for-byte.

`player.dff` is copied unchanged because CJ uses a modular body assembled from `player.img` rather than a conventional pedestrian mesh.

## Runtime files

The installed mod consists of:

- `Handies.asi`: render-time hand-profile blending and finger animation.
- `Handies.dat`: geometry-specific morph masks plus all named finger sequences sampled from the hand IFP.
- `Handies.ini`: enable/player/NPC settings, hand-sequence profiles, and ped-animation mappings.
- Final DFF/TXD files: hand geometry with the native skeleton only.

The source IFP remains `modloader\hands\ghands.ifp`; Handies does not register a second animation block in GTA. During the build, each complete finger animation is compiled into `Handies.dat` under its original IFP name. Changing only profile mappings does not require rebuilding; adding or editing an IFP sequence does.

## Hand sequence profiles

Each `[HandProfile.Name]` section chooses one IFP sequence per side:

```ini
[HandProfile.MyGesture]
Left=LHGsign1
Right=RHGsign1
TimeMode=Seconds
Loop=0
Speed=1.0
Weight=1.0
Priority=100
```

- `Left` and `Right` are animation names from `ghands.ifp`; use `None` to leave one side on the automatic relaxed/fist pose.
- `TimeMode=Seconds` follows the IFP sequence's natural timeline from the active body association.
- `TimeMode=Ped` maps the normalized duration of the body animation to the whole hand sequence.
- `Loop`, `Speed`, and `Weight` control playback; `Priority` chooses between simultaneous matching body associations.

`[PedAnimationMappings]` maps an animation group or IFP block plus animation name to a profile:

```ini
[PedAnimationMappings]
handsignal.gsign1=MyGesture
ped.ifp.FUCKU=MyOtherGesture
*.some_animation=SharedGesture
```

Matching is case-insensitive. `group.animation` targets one association group, `block.ifp.animation` targets every group backed by that IFP block, and `*` targets every group containing that animation. The included INI defines the ten native left/both-hands gang-sign mappings.

Disable the older `SanHands.asi` while using this version. The old implementation creates separate replacement hand objects and is incompatible with embedded hand geometry.

## Build and install

Run:

```powershell
.\build_handies.ps1
```

The defaults use:

- MinGW 32-bit: `C:\msys64\mingw32`
- plugin-sdk: `C:\Users\Digon\Documents\Fuentes\plugin-sdk-master`
- pose source: `C:\Users\Digon\Documents\ChatGPT\SanHands\dist\handpose.ifp`
- gang-sign source used by the installed mod: `C:\juegos\Grand Theft Auto San Andreas\modloader\hands\ghands.ifp`
- Original peds: `C:\Users\Digon\Desktop\peds`
- Native articulated hands: `C:\Users\Digon\Desktop\hands`
- Final models: `C:\Users\Digon\Desktop\peds\con manos`
- Installation: `C:\juegos\Grand Theft Auto San Andreas\modloader\Handies`

The script performs the complete pipeline: temporary authoring conversion, restoration of every native skeleton, structural validation, 32-bit ASI compilation, and installation.

## Editable blendshape GLB

The four source-hand variants and their editable profiles can be exported with Blender 5.2 or newer:

```powershell
& "C:\path\to\blender.exe" --background --factory-startup `
  --python tools\export_blendshape_glb.py -- `
  --output "C:\path\to\Handies_Blendshape_Profiles.glb"
```

The GLB contains `Slim_Left`, `Slim_Right`, `Fat_Left`, and `Fat_Right`. Its basis is the `Relaxed` runtime profile; the morph targets are `Grip`, right-hand `FuckU`, and the five matching `LHGsign` or `RHGsign` profiles sampled from the sustained part of their IFP sequences. UV coordinates and original template topology are preserved. `tools\validate_blendshape_glb.py` reimports the file in a clean Blender scene and checks all objects, vertex counts, UV layers, and non-empty shape keys.

## Manual geometry conversion

`add_hands.py` produces the temporary authoring form used to derive runtime weights:

```powershell
$env:DRAGONFF_PATH = "C:\path\to\dragonff"
python add_hands.py `
  --input "C:\path\to\peds" `
  --hands "C:\path\to\hands" `
  --output "C:\path\to\temporary-expanded" `
  --copy-txd
```

Do not install that temporary 62-bone output. `tools/finalize_runtime_models.py` must restore the native skeleton and generate `Handies.dat`; the build script does both automatically.

## Requirements

- Python 3.10 or newer.
- GTA San Andreas 1.0 US.
- [plugin-sdk](https://github.com/DK22Pac/plugin-sdk).
- [DragonFF](https://github.com/Parik27/DragonFF).
- [rwfury](https://github.com/Parik27/rwfury).
- A legally obtained copy of the required GTA assets.

This repository contains no Rockstar Games DFF, TXD, IMG, or animation assets. Generated game files are excluded from Git.

## License

SanHands 2.0 is released under the GNU General Public License v3.0 or later. Its external dependencies retain their own licenses.
