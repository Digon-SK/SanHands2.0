import argparse
from collections import Counter
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()
from gtaLib.dff import dff  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument("paths", nargs="+", type=Path)
parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
args = parser.parse_args()

for path in args.paths:
    model = dff()
    model.load_file(str(path))
    print(path.name)
    for gi, geo in enumerate(model.clumps[0].geometry_list):
        skin = geo.extensions.get("skin")
        if not skin or not geo.uv_layers:
            continue
        root = next(f for f in model.clumps[0].frame_list if f.bone_data and f.bone_data.bones)
        by_id = {bone.id: bone.index for bone in root.bone_data.bones}
        for side, ids in (("L", (34, 35, 36)), ("R", (24, 25, 26))):
            target = {by_id[i] for i in ids}
            selected = []
            mats = Counter()
            for i, (indices, weights) in enumerate(zip(skin.vertex_bone_indices, skin.vertex_bone_weights)):
                weight = sum(w for b, w in zip(indices, weights) if b in target)
                if weight > 0.25:
                    selected.append(i)
            selected_set = set(selected)
            for tri in geo.triangles:
                if selected_set.intersection((tri.a, tri.b, tri.c)):
                    mats[tri.material] += 1
            uv = [geo.uv_layers[0][i] for i in selected]
            if uv:
                print(
                    f" geo={gi} {side} n={len(uv)} "
                    f"u={min(x.u for x in uv):.3f}..{max(x.u for x in uv):.3f} "
                    f"v={min(x.v for x in uv):.3f}..{max(x.v for x in uv):.3f} mats={dict(mats)}"
                )
