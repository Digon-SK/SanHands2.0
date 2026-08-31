# SanHands 2.0

SanHands 2.0 adds articulated, animated fingers to GTA San Andreas pedestrians while preserving the game's native ped animation system.

The generated DFFs contain the replacement hand geometry, closed wrist seams, corrected UV islands, and **only the original 32-bone GTA ped skeleton**. Finger bones are used only while building per-hand morph profiles; `Handies.asi` never inserts them into GTA's HAnim hierarchy.

## Design

- Replaces the original closed hand geometry at the wrist.
- Joins the hand and forearm with bridge triangles, without gaps or overlapping open edges.
- Preserves the detailed hand UV topology and maps it into the ped's dominant skin region.
- Restores the original 32-node HAnim hierarchy and 32-bone Skin PLG before writing each final DFF.
- Keeps GTA's normal walk, fight, weapon, gesture, and body animation associations at 32 frames.
- Imports the edited `Handies_Blendshape_Profiles.glb` as the authoritative Basis and named hand shapes, rebuilding the original DFF topology from UV and position correspondence when glTF splits vertices.
- Builds geometry-specific hand ranges and transforms plus shared vertex/normal morph targets for the four slim/fat and left/right variants.
- Converts every matching animation in `ghands.ifp` into a scalar timeline that drives its same-named GLB shape.
- Selects those sequences through configurable hand profiles mapped to live ped animation associations.
- Applies the selected morph to the shared hand vertices immediately before `RpClumpRender`, renders through the untouched native 32-bone Skin, and restores the base vertices immediately afterward.
- Never creates a 62-node runtime hierarchy, swaps HAnim owners, or changes `CAnimBlendClumpData`, `finger`, or `finger01`.
- Uses the active ped association's real group, animation ID, playback time, blend amount, and configurable priority, while suppressing only the native separate replacement-hand render pass.
- Copies TXD files byte-for-byte.

`player.dff` is copied unchanged because CJ uses a modular body assembled from `player.img` rather than a conventional pedestrian mesh.

## Runtime files

The installed mod consists of:

- `Handies.asi`: render-time hand-profile blending and finger animation.
- `Handies.dat`: edited GLB positions/normals (including the `Relaxed` basis), geometry-specific hand ranges/transforms, and the timelines derived from the hand IFP. Runtime matching uses the full geometry hash first and falls back to a bounded fit of the hand surfaces, so harmless changes elsewhere in a DFF do not disable its blendshapes.
- `Handies.ini`: enable/player/NPC settings, hand-sequence profiles, and ped-animation mappings.
- Final DFF/TXD files: hand geometry with the native skeleton only.

The source IFP remains `modloader\hands\ghands.ifp`; Handies does not register a second animation block in GTA. During the build, each complete finger animation whose name matches a GLB shape is compiled into `Handies.dat`. Changing only profile mappings does not require rebuilding; editing the GLB or IFP does.

With no active hand action, the player is rendered from the GLB `Basis` without an additional morph. A ped occupying its vehicle's driver seat uses `Grip` on both hands; driver state takes priority over configurable hand-signal profiles until the ped leaves that seat.

Outside the driver seat, an equipped weapon applies the edited GLB `Weap` target to the right hand as the final hand pose. The target is used only by hand templates that actually provide it; the current edited GLB defines it on `Slim_Right`.

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
- edited blendshape source: `C:\Users\Digon\Desktop\ExpreSA\Handies_Blendshape_Profiles.glb`
- Blender: `C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe`
- Original peds: `C:\Users\Digon\Desktop\peds`
- Native articulated hands: `C:\Users\Digon\Desktop\hands`
- Final models: `C:\Users\Digon\Desktop\peds\con manos`
- Installation: `C:\juegos\Grand Theft Auto San Andreas\modloader\Handies`

The script extracts and validates the edited GLB, performs the temporary authoring conversion, restores every native skeleton, validates the complete output, compiles the 32-bit ASI, and installs it.

## Editable blendshape GLB

The four source-hand variants and their editable profiles can be exported with Blender 5.2 or newer:

```powershell
& "C:\path\to\blender.exe" --background --factory-startup `
  --python tools\export_blendshape_glb.py -- `
  --output "C:\path\to\Handies_Blendshape_Profiles.glb"
```

The GLB contains `Slim_Left`, `Slim_Right`, `Fat_Left`, and `Fat_Right`. Its basis is the `Relaxed` runtime profile; the morph targets are `Grip`, right-hand `FuckU`, `Slim_Right.Weap`, and the five matching `LHGsign` or `RHGsign` profiles sampled from the sustained part of their IFP sequences. `tools\extract_blendshape_profiles.py` accepts ordinary glTF vertex splitting, ignores unrelated scene objects, reconstructs the original source topology, and writes the runtime-ready profiles. UV coordinates and the editable shape keys are preserved.

## Manual geometry conversion

`add_hands.py` produces the temporary authoring form used to derive runtime weights:

```powershell
$env:DRAGONFF_PATH = "C:\path\to\dragonff"
python add_hands.py `
  --input "C:\path\to\peds" `
  --hands "C:\path\to\hands" `
  --output "C:\path\to\temporary-expanded" `
  --blendshapes "C:\path\to\BlendshapeProfiles.dat" `
  --manifest "C:\path\to\expanded-manifest.json" `
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
