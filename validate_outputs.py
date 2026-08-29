import argparse
import hashlib
import struct
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()
from gtaLib.dff import dff  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
args = parser.parse_args()

source = args.source
output = args.output
expected_new_ids = set(range(1005, 1018)) | set(range(1105, 1118))


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.digest()


dff_paths = sorted(output.glob("*.dff"))
txd_paths = sorted(output.glob("*.txd"))
errors = []
geometries = 0

expected_dff = len(list(source.glob("*.dff")))
expected_txd = len(list(source.glob("*.txd")))
if len(dff_paths) != expected_dff:
    errors.append(f"DFF count is {len(dff_paths)}, expected {expected_dff}")
if len(txd_paths) != expected_txd:
    errors.append(f"TXD count is {len(txd_paths)}, expected {expected_txd}")

for path in dff_paths:
    raw = path.read_bytes()
    if len(raw) < 12 or struct.unpack_from("<I", raw, 4)[0] + 12 != len(raw):
        errors.append(f"{path.name}: invalid root chunk length")
        continue
    try:
        model = dff()
        model.load_memory(raw)
        if path.name.casefold() == "player.dff":
            if digest(path) != digest(source / path.name):
                errors.append("player.dff: expected byte-identical modular skeleton")
            continue
        for clump in model.clumps:
            roots = [frame for frame in clump.frame_list if frame.bone_data and frame.bone_data.bones]
            if len(roots) != len(clump.geometry_list):
                errors.append(f"{path.name}: root/geometry count mismatch")
            for root in roots:
                ids = {bone.id for bone in root.bone_data.bones}
                if len(root.bone_data.bones) != 58 or root.bone_data.header.bone_count != 58:
                    errors.append(f"{path.name}: HAnim root does not contain 58 bones")
                if not expected_new_ids.issubset(ids):
                    errors.append(f"{path.name}: native finger bone IDs are incomplete")
            for frame_index, frame in enumerate(clump.frame_list):
                if frame.parent >= len(clump.frame_list):
                    errors.append(f"{path.name}: frame {frame_index} has invalid parent")
            for geo in clump.geometry_list:
                geometries += 1
                skin = geo.extensions.get("skin")
                count = len(geo.vertices)
                if skin is None or skin.num_bones != 58 or len(skin.bone_matrices) != 58:
                    errors.append(f"{path.name}: invalid 58-bone Skin PLG")
                    continue
                if len(skin.vertex_bone_indices) != count or len(skin.vertex_bone_weights) != count:
                    errors.append(f"{path.name}: skin/vertex mismatch")
                if geo.normals and len(geo.normals) != count:
                    errors.append(f"{path.name}: normal/vertex mismatch")
                if any(len(layer) != count for layer in geo.uv_layers):
                    errors.append(f"{path.name}: UV/vertex mismatch")
                if any(max(indices) >= 58 for indices in skin.vertex_bone_indices):
                    errors.append(f"{path.name}: out-of-range skin index")
                if any(max(tri.a, tri.b, tri.c) >= count for tri in geo.triangles):
                    errors.append(f"{path.name}: out-of-range triangle index")
    except Exception as exc:
        errors.append(f"{path.name}: parse failure: {exc}")

for path in txd_paths:
    original = source / path.name
    if not original.exists() or digest(path) != digest(original):
        errors.append(f"{path.name}: texture differs from source")

print(
    f"VALIDATION dff={len(dff_paths)} txd={len(txd_paths)} "
    f"geometries={geometries} errors={len(errors)}"
)
for error in errors[:100]:
    print(f"ERROR {error}")
if errors:
    raise SystemExit(1)
