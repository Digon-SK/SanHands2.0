import argparse
from collections import Counter, defaultdict
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()

from gtaLib.dff import dff  # noqa: E402


parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
args = parser.parse_args()

root = args.root
stats = Counter()
examples = defaultdict(list)

for path in sorted(root.glob("*.dff")):
    try:
        model = dff()
        model.load_file(str(path))
        if len(model.clumps) != 1:
            key = f"clumps:{len(model.clumps)}"
        else:
            clump = model.clumps[0]
            if len(clump.geometry_list) != 1:
                key = f"geometries:{len(clump.geometry_list)}"
            else:
                geo = clump.geometry_list[0]
                skin = geo.extensions.get("skin")
                roots = [f for f in clump.frame_list if f.bone_data and f.bone_data.bones]
                ids = {f.bone_data.header.id for f in clump.frame_list if f.bone_data}
                if not skin:
                    key = "no_skin"
                elif not roots:
                    key = "no_hanim_root"
                elif not {23, 24, 33, 34}.issubset(ids):
                    missing = sorted({23, 24, 33, 34} - ids)
                    key = f"missing_hand_ids:{','.join(map(str, missing))}"
                elif len(skin.vertex_bone_indices) != len(geo.vertices):
                    key = "skin_vertex_mismatch"
                else:
                    key = f"supported:bones={skin.num_bones}:frames={len(clump.frame_list)}"
        stats[key] += 1
        if len(examples[key]) < 12:
            examples[key].append(path.name)
    except Exception as exc:
        key = f"error:{type(exc).__name__}:{exc}"
        stats[key] += 1
        if len(examples[key]) < 12:
            examples[key].append(path.name)

for key, count in sorted(stats.items()):
    print(f"{count:3d} {key}: {', '.join(examples[key])}")
