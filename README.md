# SanHands 2.0

Batch conversion tool for GTA San Andreas pedestrian DFF models. It removes the original closed hand mesh at each wrist, inserts Rockstar's articulated `fhandl`/`fhandr` or `shandl`/`shandr` geometry, and extends the pedestrian skin with weighted finger bones.

The converter selects the small or standard native hand according to each skeleton's proportions. It stitches the detailed eight-vertex wrist loop to the original low-poly forearm contour, even when the two loops have different vertex counts. The native hand UV island is remapped coherently into the pedestrian's dominant skin region so the fingers do not sample unrelated parts of the texture.

## What it changes

- Replaces both original hand meshes at the wrist.
- Closes the wrist with shared bridge triangles instead of leaving overlapping open edges.
- Preserves the detailed hand's UV topology and assigns one dominant skin material per hand.
- Preserves the standard forearm, hand, and two GTA finger bone IDs.
- Adds 13 additional weighted finger bones per hand.
- Produces a 58-bone skin for conventional pedestrians.
- Copies matching TXD files without modifying them.
- Handles DFFs containing more than one skinned geometry.

`player.dff` is copied unchanged. CJ uses a modular body assembled from `player.img`, and the base DFF has no conventional hand surface to replace.

## Handies ASI

`src/Handies.cpp` animates the added finger bones inside each converted ped. It
does not create replacement `CHandObject` instances and does not hide the ped's
embedded hands.

GTA builds its standard animation associations against the original 32-node
`male01` skeleton. When those associations are copied to a converted 58-node
ped, the original game keeps a 32-element blend-node array while its update loop
walks all 58 frames. `Handies.asi` intercepts that update call and rebuilds any
mismatched association against the actual clump, letting the game's own
bone-tag lookup remap body tracks without changing their animations.

The plugin loads `Handies.ifp` as two partial animations. These contain only the
15 finger tracks for each side, remapped to the IDs emitted by `add_hands.py`.
The original wrist, arm, body, weapon, and ragdoll transforms therefore continue
to come from GTA and other animation mods.

Build and install the complete local mod with:

```powershell
.\build_handies.ps1
```

Defaults target GTA SA 1.0 US, MinGW 32-bit at `C:\msys64\mingw32`, plugin-sdk
at `C:\Users\Digon\Documents\Fuentes\plugin-sdk-master`, and installation at
`C:\juegos\Grand Theft Auto San Andreas\modloader\Handies`. The generated IFP
is derived locally from `SanHands\dist\handpose.ifp`; no game animation assets
are committed to this repository.

Disable the older `SanHands.asi` while using Handies. The older plugin creates
separate hand objects by design, so loading both implementations would display
duplicate systems.

## Requirements

- Python 3.10 or newer.
- A legally obtained PC copy of GTA San Andreas.
- [DragonFF](https://github.com/Parik27/DragonFF), extracted locally. DragonFF's standalone `gtaLib/dff.py` module is used to read and write RenderWare files.
- The four native articulated hand files: `fhandl.dff`, `fhandr.dff`, `shandl.dff`, and `shandr.dff`.

This repository intentionally contains no Rockstar Games assets. You must supply the DFF/TXD files from your own game installation.

## Usage

```powershell
python add_hands.py `
  --dragonff "C:\tools\DragonFF-master" `
  --input "C:\path\to\peds" `
  --hands "C:\path\to\hands" `
  --output "C:\path\to\peds-with-hands" `
  --copy-txd
```

The input directory is read non-recursively, so an output directory located inside it is not processed again. Existing source DFF/TXD files are never edited.

To validate a completed batch independently:

```powershell
python validate_outputs.py `
  --dragonff "C:\tools\DragonFF-master" `
  --source "C:\path\to\peds" `
  --hands "C:\path\to\hands" `
  --output "C:\path\to\peds-with-hands"
```

Passing `--hands` enables additional checks for closed wrist bridges, finite UVs, and a single material on each replacement hand.

Diagnostic helpers are also included:

- `analyze_dff.py`: inspect frames, HAnim data, geometry, materials, and skin weights.
- `audit_peds.py`: audit a directory for compatible pedestrian skeletons.
- `inspect_hand_uv.py`: inspect original hand UV ranges and materials.

## Important

The generated DFF files pass structural round-trip validation through DragonFF, including frame parents, HAnim roots, skin matrices, weights, UV counts, and triangle indices. Test generated models in a separate mod-loader directory before replacing any installed game files.

## License

SanHands 2.0 is released under the GNU General Public License v3.0 or later. DragonFF is a separate project distributed under its own GPL license.
