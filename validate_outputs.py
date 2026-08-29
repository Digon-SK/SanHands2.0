import argparse
import hashlib
import math
import struct
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()
from gtaLib.dff import dff  # noqa: E402
import add_hands as ah  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--hands", type=Path)
parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
parser.add_argument("--runtime-data", type=Path, required=True)
args = parser.parse_args()

source = args.source
output = args.output
runtime_only_ids = set(range(1005, 1018)) | set(range(1105, 1118))
templates = None
if args.hands:
    templates = {
        (variant, side.upper()): ah.load_template(args.hands / f"{variant}hand{side}.dff")
        for variant in ("f", "s")
        for side in ("l", "r")
    }


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
geometry_profiles = set()

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
        native_model = dff()
        native_model.load_file(str(source / path.name))
        if len(model.clumps) != len(native_model.clumps):
            errors.append(f"{path.name}: native clump count changed")
            continue
        for clump_index, (clump, native_clump) in enumerate(
            zip(model.clumps, native_model.clumps)
        ):
            if len(clump.frame_list) != len(native_clump.frame_list):
                errors.append(f"{path.name}: native frame count changed")
            else:
                for frame_index, (frame, native_frame) in enumerate(
                    zip(clump.frame_list, native_clump.frame_list)
                ):
                    if frame.parent != native_frame.parent:
                        errors.append(
                            f"{path.name}: frame {frame_index} parent changed"
                        )
                    output_bones = (
                        [] if frame.bone_data is None else
                        [(bone.id, bone.index, bone.type) for bone in frame.bone_data.bones]
                    )
                    native_bones = (
                        [] if native_frame.bone_data is None else
                        [
                            (bone.id, bone.index, bone.type)
                            for bone in native_frame.bone_data.bones
                        ]
                    )
                    if output_bones != native_bones:
                        errors.append(
                            f"{path.name}: frame {frame_index} native HAnim changed"
                        )
            roots = [frame for frame in clump.frame_list if frame.bone_data and frame.bone_data.bones]
            if len(roots) != len(clump.geometry_list):
                errors.append(f"{path.name}: root/geometry count mismatch")
            for root in roots:
                ids = {bone.id for bone in root.bone_data.bones}
                if len(root.bone_data.bones) != 32 or root.bone_data.header.bone_count != 32:
                    errors.append(f"{path.name}: HAnim root is not the native 32-bone rig")
                if runtime_only_ids.intersection(ids):
                    errors.append(f"{path.name}: runtime-only finger IDs remained in DFF")
            for frame_index, frame in enumerate(clump.frame_list):
                if frame.parent >= len(clump.frame_list):
                    errors.append(f"{path.name}: frame {frame_index} has invalid parent")
            if len(clump.geometry_list) != len(native_clump.geometry_list):
                errors.append(f"{path.name}: native geometry count changed")
                continue
            for geometry_index, (geo, native_geo) in enumerate(
                zip(clump.geometry_list, native_clump.geometry_list)
            ):
                geometries += 1
                skin = geo.extensions.get("skin")
                count = len(geo.vertices)
                geometry_hash = 0xCBF29CE484222325
                for vertex in geo.vertices:
                    for byte in struct.pack("<3f", vertex.x, vertex.y, vertex.z):
                        geometry_hash ^= byte
                        geometry_hash = (geometry_hash * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
                geometry_profiles.add((geometry_hash, count))
                if skin is None or skin.num_bones != 32 or len(skin.bone_matrices) != 32:
                    errors.append(f"{path.name}: invalid native 32-bone Skin PLG")
                    continue
                native_skin = native_geo.extensions.get("skin")
                if native_skin is None or any(
                    abs(value - native_value) > 1.0e-5
                    for matrix, native_matrix in zip(
                        skin.bone_matrices, native_skin.bone_matrices
                    )
                    for row, native_row in zip(matrix, native_matrix)
                    for value, native_value in zip(row, native_row)
                ):
                    errors.append(
                        f"{path.name}: geometry {geometry_index} native bind matrices changed"
                    )
                if len(skin.vertex_bone_indices) != count or len(skin.vertex_bone_weights) != count:
                    errors.append(f"{path.name}: skin/vertex mismatch")
                if geo.normals and len(geo.normals) != count:
                    errors.append(f"{path.name}: normal/vertex mismatch")
                if any(len(layer) != count for layer in geo.uv_layers):
                    errors.append(f"{path.name}: UV/vertex mismatch")
                if any(max(indices) >= 32 for indices in skin.vertex_bone_indices):
                    errors.append(f"{path.name}: out-of-range skin index")
                if any(max(tri.a, tri.b, tri.c) >= count for tri in geo.triangles):
                    errors.append(f"{path.name}: out-of-range triangle index")
                if any(
                    not math.isfinite(value)
                    for layer in geo.uv_layers
                    for uv in layer
                    for value in (uv.u, uv.v)
                ):
                    errors.append(f"{path.name}: non-finite UV coordinate")

                if templates:
                    _, id_to_frame = ah.skeleton_for_geometry(clump, geometry_index)
                    selected_templates = []
                    for side in ("L", "R"):
                        hand_frame = clump.frame_list[id_to_frame[ah.SIDES[side]["hand"]]]
                        selected_templates.append(
                            ah.choose_template(templates, side, ah.vector_length(hand_frame.position))
                        )
                    hand_counts = [len(item["geo"].vertices) for item in selected_templates]
                    original_count = count - sum(hand_counts)
                    if original_count <= 0:
                        errors.append(f"{path.name}: cannot locate appended hand ranges")
                    start = original_count
                    for side, hand_count in zip(("L", "R"), hand_counts):
                        hand_range = set(range(start, start + hand_count))
                        bridge_triangles = [
                            tri
                            for tri in geo.triangles
                            if hand_range.intersection((tri.a, tri.b, tri.c))
                            and any(index < original_count for index in (tri.a, tri.b, tri.c))
                        ]
                        if len(bridge_triangles) < 3:
                            errors.append(f"{path.name}: {side} wrist is not stitched")
                        if any(len({tri.a, tri.b, tri.c}) != 3 for tri in bridge_triangles):
                            errors.append(f"{path.name}: {side} wrist has a degenerate bridge")
                        hand_materials = {
                            tri.material
                            for tri in geo.triangles
                            if all(index in hand_range for index in (tri.a, tri.b, tri.c))
                        }
                        if len(hand_materials) != 1:
                            errors.append(
                                f"{path.name}: {side} hand uses {len(hand_materials)} materials"
                            )
                        start += hand_count
    except Exception as exc:
        errors.append(f"{path.name}: parse failure: {exc}")

for path in txd_paths:
    original = source / path.name
    if not original.exists() or digest(path) != digest(original):
        errors.append(f"{path.name}: texture differs from source")

try:
    data = args.runtime_data.read_bytes()
    if len(data) < 16 or data[:8] != b"HND2DAT\0":
        raise ValueError("invalid magic")
    version, profile_count = struct.unpack_from("<II", data, 8)
    if version != 2:
        raise ValueError(f"unsupported version {version}")
    offset = 16 + 2 * 3 * 15 * 16
    for _side in range(2):
        for _signal in range(5):
            (duration,) = struct.unpack_from("<f", data, offset)
            offset += 4
            if duration <= 0.0 or duration > 30.0:
                raise ValueError(f"invalid hand-signal duration {duration}")
            for _bone in range(15):
                (key_count,) = struct.unpack_from("<I", data, offset)
                offset += 4
                if key_count == 0 or key_count > 64:
                    raise ValueError(f"invalid hand-signal key count {key_count}")
                previous_time = -1.0
                for _key in range(key_count):
                    time, x, y, z, w = struct.unpack_from("<5f", data, offset)
                    offset += 20
                    if time < previous_time or time < 0.0 or time > duration + 1.0e-4:
                        raise ValueError(f"invalid hand-signal key time {time}")
                    length = math.sqrt(x * x + y * y + z * z + w * w)
                    if abs(length - 1.0) > 1.0e-3:
                        raise ValueError("non-normalized hand-signal quaternion")
                    previous_time = time
    data_profiles = set()
    for _ in range(profile_count):
        geometry_hash, vertex_count, bone_count = struct.unpack_from(
            "<QII", data, offset
        )
        if bone_count != 58:
            raise ValueError(f"runtime profile has {bone_count} bones")
        data_profiles.add((geometry_hash, vertex_count))
        offset += 16 + 30 * 12 + vertex_count * 4 + vertex_count * 16 + 58 * 64
    if offset != len(data):
        raise ValueError(f"trailing/truncated bytes: parsed {offset}, file {len(data)}")
    if data_profiles != geometry_profiles:
        missing = len(geometry_profiles - data_profiles)
        extra = len(data_profiles - geometry_profiles)
        raise ValueError(f"profile mismatch: missing={missing} extra={extra}")
except Exception as exc:
    errors.append(f"Handies.dat: {exc}")

print(
    f"VALIDATION dff={len(dff_paths)} txd={len(txd_paths)} "
    f"geometries={geometries} errors={len(errors)}"
)
for error in errors[:100]:
    print(f"ERROR {error}")
if errors:
    raise SystemExit(1)
