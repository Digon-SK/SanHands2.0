# SanHands 2.0

Batch conversion tool for GTA San Andreas pedestrian DFF models. It removes the original closed hand mesh at each wrist, inserts Rockstar's articulated `fhandl`/`fhandr` or `shandl`/`shandr` geometry, and extends the pedestrian skin with weighted finger bones.

The converter selects the small or standard native hand according to each skeleton's proportions. UV coordinates and material assignments are transferred from the original hand surface so every pedestrian continues using its own TXD.

## What it changes

- Replaces both original hand meshes at the wrist.
- Preserves the standard forearm, hand, and two GTA finger bone IDs.
- Adds 13 additional weighted finger bones per hand.
- Produces a 58-bone skin for conventional pedestrians.
- Copies matching TXD files without modifying them.
- Handles DFFs containing more than one skinned geometry.

`player.dff` is copied unchanged. CJ uses a modular body assembled from `player.img`, and the base DFF has no conventional hand surface to replace.

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
  --output "C:\path\to\peds-with-hands"
```

Diagnostic helpers are also included:

- `analyze_dff.py`: inspect frames, HAnim data, geometry, materials, and skin weights.
- `audit_peds.py`: audit a directory for compatible pedestrian skeletons.
- `inspect_hand_uv.py`: inspect original hand UV ranges and materials.

## Important

The generated DFF files pass structural round-trip validation through DragonFF, including frame parents, HAnim roots, skin matrices, weights, UV counts, and triangle indices. Test generated models in a separate mod-loader directory before replacing any installed game files.

## License

SanHands 2.0 is released under the GNU General Public License v3.0 or later. DragonFF is a separate project distributed under its own GPL license.
