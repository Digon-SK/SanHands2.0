#!/usr/bin/env python3
"""Build native 32-bone ped DFFs plus the runtime finger data for Handies."""

from __future__ import annotations

import argparse
import copy
import json
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"HND2DAT\0"
VERSION = 5
NATIVE_BONES = 32
RUNTIME_BONES = 62
FINGER_IDS = tuple(range(3, 18))
@dataclass(frozen=True)
class RuntimeProfile:
    geometry_hash: int
    vertex_count: int
    hands: tuple["RuntimeHand", "RuntimeHand"]


@dataclass(frozen=True)
class RuntimeHand:
    start: int
    count: int
    template_index: int
    transform: tuple[float, ...]


@dataclass(frozen=True)
class MorphWeightKey:
    time: float
    weight: float


@dataclass(frozen=True)
class MorphAnimation:
    name: str
    duration: float
    keys: tuple[MorphWeightKey, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--expanded", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--pose-source", required=True, type=Path)
    parser.add_argument("--gang-source", required=True, type=Path)
    parser.add_argument("--dragonff", required=True, type=Path)
    parser.add_argument("--rwfury-root", required=True, type=Path)
    parser.add_argument("--blendshapes", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--one", help="Process only one DFF filename")
    return parser.parse_args()


def fnv1a_vertices(vertices) -> int:
    value = 0xCBF29CE484222325
    for vertex in vertices:
        for byte in struct.pack("<3f", vertex.x, vertex.y, vertex.z):
            value ^= byte
            value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def normalize_quaternion(value: tuple[float, float, float, float]):
    length = math.sqrt(sum(component * component for component in value))
    if length <= 1.0e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(component / length for component in value)


def interpolate_quaternion(a, b, amount):
    if sum(x * y for x, y in zip(a, b)) < 0.0:
        b = tuple(-component for component in b)
    return normalize_quaternion(
        tuple(x + (y - x) * amount for x, y in zip(a, b))
    )


def sample_rotation(obj, time):
    frames = sorted(obj.frames, key=lambda frame: frame.time)
    if not frames:
        raise ValueError(f"{obj.name}: animation track without frames")
    if time <= frames[0].time:
        return normalize_quaternion(tuple(frames[0].rotation))
    if time >= frames[-1].time:
        return normalize_quaternion(tuple(frames[-1].rotation))
    for first, second in zip(frames, frames[1:]):
        if first.time <= time <= second.time:
            span = second.time - first.time
            amount = 0.0 if span <= 1.0e-12 else (time - first.time) / span
            return interpolate_quaternion(
                tuple(first.rotation), tuple(second.rotation), amount
            )
    raise AssertionError("unreachable quaternion sample")


POSE_TIMES = (0.56, 0.6666667, 1.3333333)


def load_pose_table(pose_source: Path, rwfury_root: Path):
    sys.path.insert(0, str(rwfury_root.resolve()))
    from rwfury import Ifp

    package = Ifp.from_bytes(pose_source.read_bytes())
    result = []
    for animation_name in ("LHGrip", "RHGrip"):
        animation = package.get_animation(animation_name)
        if animation is None:
            raise ValueError(f"Missing animation {animation_name}")
        by_id = {obj.bone_id: obj for obj in animation.objects}
        if not all(bone_id in by_id for bone_id in FINGER_IDS):
            raise ValueError(f"{animation_name}: incomplete finger tracks")
        result.append(
            tuple(
                tuple(sample_rotation(by_id[bone_id], time) for bone_id in FINGER_IDS)
                for time in POSE_TIMES
            )
        )
    return tuple(result)


def load_hand_animation_table(gang_source: Path, rwfury_root: Path):
    """Load every named IFP sequence containing the complete finger rig."""
    sys.path.insert(0, str(rwfury_root.resolve()))
    from rwfury import Ifp

    package = Ifp.from_bytes(gang_source.read_bytes())
    if package.internal_name.upper() != "GHANDS":
        raise ValueError(f"{gang_source}: expected the GHANDS animation package")

    result = []
    names = set()
    for animation in package.animations:
        by_id = {obj.bone_id: obj for obj in animation.objects}
        if not all(bone_id in by_id for bone_id in FINGER_IDS):
            continue
        encoded_name = animation.name.encode("ascii")
        if not encoded_name or len(encoded_name) > 63:
            raise ValueError(f"Invalid hand animation name {animation.name!r}")
        folded_name = animation.name.casefold()
        if folded_name in names:
            raise ValueError(f"Duplicate hand animation {animation.name}")
        names.add(folded_name)

        tracks = []
        duration = 0.0
        for bone_id in FINGER_IDS:
            frames = sorted(by_id[bone_id].frames, key=lambda frame: frame.time)
            if not frames or len(frames) > 64:
                raise ValueError(
                    f"{animation.name}: invalid key count for bone {bone_id}"
                )
            keys = tuple(
                (float(frame.time), normalize_quaternion(tuple(frame.rotation)))
                for frame in frames
            )
            duration = max(duration, keys[-1][0])
            tracks.append(keys)
        if duration <= 0.0 or duration > 30.0:
            raise ValueError(f"{animation.name}: invalid duration {duration}")
        result.append((animation.name, duration, tuple(tracks)))

    if not result:
        raise ValueError(f"{gang_source}: no complete finger animation sequences")
    return tuple(result)


def sample_key_track(keys, time):
    if time <= keys[0][0]:
        return keys[0][1]
    if time >= keys[-1][0]:
        return keys[-1][1]
    for first, second in zip(keys, keys[1:]):
        if first[0] <= time <= second[0]:
            span = second[0] - first[0]
            amount = 0.0 if span <= 1.0e-12 else (time - first[0]) / span
            return interpolate_quaternion(first[1], second[1], amount)
    raise AssertionError("unreachable key track")


def aligned_quaternion(value, reference):
    return tuple(-item for item in value) if sum(
        item * other for item, other in zip(value, reference)
    ) < 0.0 else value


def make_morph_animations(hand_animations, available_targets, sample_time=1.0):
    result = []
    for name, duration, tracks in hand_animations:
        if name.casefold() not in available_targets:
            continue
        times = sorted({time for track in tracks for time, _ in track})
        starts = [track[0][1] for track in tracks]
        targets = [sample_key_track(track, sample_time) for track in tracks]
        keys = []
        for time in times:
            numerator = 0.0
            denominator = 0.0
            for track, start, target in zip(tracks, starts, targets):
                target = aligned_quaternion(target, start)
                current = aligned_quaternion(sample_key_track(track, time), start)
                direction = tuple(b - a for a, b in zip(start, target))
                delta = tuple(value - base for value, base in zip(current, start))
                numerator += sum(value * axis for value, axis in zip(delta, direction))
                denominator += sum(axis * axis for axis in direction)
            weight = 0.0 if denominator <= 1.0e-12 else numerator / denominator
            keys.append(MorphWeightKey(float(time), min(max(weight, 0.0), 1.0)))
        result.append(MorphAnimation(name, duration, tuple(keys)))
    if not result:
        raise ValueError("No IFP animations match blendshape targets")
    return tuple(result)


def descendants(frames, root_index):
    result = []
    pending = [root_index]
    while pending:
        current = pending.pop()
        result.append(current)
        pending.extend(
            index for index, frame in enumerate(frames) if frame.parent == current
        )
    return result


def skeleton_for_geometry(clump, geometry_index):
    atomic = next(
        (item for item in clump.atomic_list if item.geometry == geometry_index), None
    )
    if atomic is None:
        raise ValueError(f"No atomic for geometry {geometry_index}")
    current = atomic.frame
    visited = set()
    root_index = None
    while current >= 0 and current not in visited:
        visited.add(current)
        frame = clump.frame_list[current]
        if frame.bone_data and frame.bone_data.bones:
            root_index = current
            break
        current = frame.parent
    if root_index is None:
        candidates = descendants(clump.frame_list, atomic.frame)
        roots = [
            index
            for index in candidates
            if clump.frame_list[index].bone_data
            and clump.frame_list[index].bone_data.bones
        ]
        if len(roots) != 1:
            raise ValueError(
                f"Expected one HAnim root for geometry {geometry_index}, got {roots}"
            )
        root_index = roots[0]
    id_to_frame = {
        clump.frame_list[index].bone_data.header.id: clump.frame_list[index]
        for index in descendants(clump.frame_list, root_index)
        if clump.frame_list[index].bone_data is not None
    }
    return clump.frame_list[root_index], id_to_frame


def rw_matrix_values(matrix) -> tuple[float, ...]:
    return (
        matrix[0][0], matrix[1][0], matrix[2][0], 0.0,
        matrix[0][1], matrix[1][1], matrix[2][1], 0.0,
        matrix[0][2], matrix[1][2], matrix[2][2], 0.0,
        matrix[0][3], matrix[1][3], matrix[2][3], 1.0,
    )


def make_profile(geometry, report, template_indices, templates) -> RuntimeProfile:
    skin = geometry.extensions.get("skin")
    if skin is None or skin.num_bones != RUNTIME_BONES:
        raise ValueError("Expanded geometry does not have a 62-bone skin")
    if len(skin.bone_matrices) != RUNTIME_BONES:
        raise ValueError("Expanded skin matrix count is not 62")
    hands = []
    for expected_side, hand in zip(("L", "R"), report["hands"]):
        if hand["side"] != expected_side:
            raise ValueError("Blendshape hand manifest side mismatch")
        template_index = template_indices[hand["template"].casefold()]
        template = templates[template_index]
        start = int(hand["start"])
        count = int(hand["count"])
        if count != template.vertex_count or start < 0 or start + count > len(geometry.vertices):
            raise ValueError("Blendshape hand manifest range mismatch")
        hands.append(RuntimeHand(
            start=start,
            count=count,
            template_index=template_index,
            transform=rw_matrix_values(hand["transform"]),
        ))
    return RuntimeProfile(
        geometry_hash=fnv1a_vertices(geometry.vertices),
        vertex_count=len(geometry.vertices),
        hands=tuple(hands),
    )


def collapsed_bone_index(expanded_index: int, expanded_index_to_id, native_id_to_index):
    bone_id = expanded_index_to_id[expanded_index]
    if 1003 <= bone_id <= 1017:
        return native_id_to_index[34]
    if 1103 <= bone_id <= 1117:
        return native_id_to_index[24]
    return native_id_to_index[bone_id]


def collapse_vertex(indices, weights, expanded_index_to_id, native_id_to_index):
    combined = {}
    for expanded_index, weight in zip(indices, weights):
        if weight <= 0.0:
            continue
        native_index = collapsed_bone_index(
            expanded_index, expanded_index_to_id, native_id_to_index
        )
        combined[native_index] = combined.get(native_index, 0.0) + weight
    ordered = sorted(combined.items(), key=lambda item: item[1], reverse=True)[:4]
    total = sum(weight for _, weight in ordered)
    if total <= 1.0e-12:
        ordered = [(0, 1.0)]
        total = 1.0
    out_indices = [index for index, _ in ordered]
    out_weights = [weight / total for _, weight in ordered]
    while len(out_indices) < 4:
        out_indices.append(0)
        out_weights.append(0.0)
    return tuple(out_indices), tuple(out_weights)


def restore_native_skeleton(expanded_clump, native_clump):
    if len(expanded_clump.geometry_list) != len(native_clump.geometry_list):
        raise ValueError("Source/expanded geometry count mismatch")

    for geometry_index, (geometry, native_geometry) in enumerate(zip(
        expanded_clump.geometry_list, native_clump.geometry_list
    )):
        expanded_root, _ = skeleton_for_geometry(expanded_clump, geometry_index)
        native_root, _ = skeleton_for_geometry(native_clump, geometry_index)
        expanded_index_to_id = {
            bone.index: bone.id for bone in expanded_root.bone_data.bones
        }
        native_id_to_index = {
            bone.id: bone.index for bone in native_root.bone_data.bones
        }
        if len(native_root.bone_data.bones) != NATIVE_BONES:
            raise ValueError("Source model is not a native 32-bone ped")
        skin = geometry.extensions.get("skin")
        native_skin = native_geometry.extensions.get("skin")
        if skin is None or native_skin is None:
            raise ValueError("Missing Skin PLG while restoring native skeleton")
        collapsed_indices = []
        collapsed_weights = []
        for indices, weights in zip(
            skin.vertex_bone_indices, skin.vertex_bone_weights
        ):
            new_indices, new_weights = collapse_vertex(
                indices, weights, expanded_index_to_id, native_id_to_index
            )
            collapsed_indices.append(new_indices)
            collapsed_weights.append(new_weights)
        skin.vertex_bone_indices = collapsed_indices
        skin.vertex_bone_weights = collapsed_weights
        skin.num_bones = NATIVE_BONES
        skin.bone_matrices = copy.deepcopy(native_skin.bone_matrices)
        skin.bones_used = []
        skin.max_weights_per_vertex = 0
        geometry.extensions.pop("mat_split", None)

    expanded_clump.frame_list = copy.deepcopy(native_clump.frame_list)


def write_runtime_data(path: Path, templates, hand_animations, profiles):
    data = bytearray(MAGIC)
    data += struct.pack("<II", VERSION, len(profiles))
    data += struct.pack("<I", len(templates))
    for template in templates:
        encoded_template = template.name.encode("ascii")
        targets = [target for target in template.targets if target.name.casefold() != "relaxed"]
        data += struct.pack(
            "<III", len(encoded_template), template.vertex_count, len(targets)
        )
        data += encoded_template
        for target in targets:
            encoded_target = target.name.encode("ascii")
            data += struct.pack("<I", len(encoded_target))
            data += encoded_target
            for position in target.positions:
                data += struct.pack("<3f", *position)
            for normal in target.normals:
                data += struct.pack("<3f", *normal)
    data += struct.pack("<I", len(hand_animations))
    for animation in hand_animations:
        encoded_name = animation.name.encode("ascii")
        data += struct.pack("<I", len(encoded_name))
        data += encoded_name
        data += struct.pack("<fI", animation.duration, len(animation.keys))
        for key in animation.keys:
            data += struct.pack("<2f", key.time, key.weight)
    for profile in profiles:
        data += struct.pack("<QI", profile.geometry_hash, profile.vertex_count)
        for hand in profile.hands:
            data += struct.pack(
                "<III", hand.start, hand.count, hand.template_index
            )
            data += struct.pack("<16f", *hand.transform)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def validate_runtime_file(path, dff_type):
    model = dff_type()
    model.load_file(str(path))
    for clump in model.clumps:
        for geometry_index, geometry in enumerate(clump.geometry_list):
            root, _ = skeleton_for_geometry(clump, geometry_index)
            if len(root.bone_data.bones) != NATIVE_BONES:
                raise ValueError(f"{path.name}: expected native 32-bone HAnim")
            skin = geometry.extensions.get("skin")
            if skin is None or skin.num_bones != NATIVE_BONES:
                raise ValueError(f"{path.name}: expected native 32-bone skin")
            if len(skin.bone_matrices) != NATIVE_BONES:
                raise ValueError(f"{path.name}: invalid native matrix count")
            if max(max(indices) for indices in skin.vertex_bone_indices) >= NATIVE_BONES:
                raise ValueError(f"{path.name}: expanded weight remained in DFF")


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(args.dragonff.resolve()))
    from gtaLib.dff import dff
    from blendshape_profiles import load_profiles

    templates = load_profiles(args.blendshapes)
    template_indices = {
        template.name.casefold(): index for index, template in enumerate(templates)
    }
    available_targets = {
        target.name.casefold()
        for template in templates
        for target in template.targets
    }
    hand_animations = make_morph_animations(
        load_hand_animation_table(args.gang_source, args.rwfury_root),
        available_targets,
    )
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("version") != 1:
        raise ValueError("Unsupported expanded-model manifest")
    args.output.mkdir(parents=True, exist_ok=True)
    profiles_by_hash = {}
    count = 0

    expanded_paths = (
        [args.expanded / args.one]
        if args.one
        else sorted(args.expanded.glob("*.dff"))
    )
    for expanded_path in expanded_paths:
        source_path = args.source / expanded_path.name
        if not source_path.is_file():
            raise FileNotFoundError(source_path)
        expanded = dff()
        expanded.load_file(str(expanded_path))
        native = dff()
        native.load_file(str(source_path))

        if expanded_path.name.casefold() == "player.dff":
            # DragonFF exposes the native bin-mesh plugin as a raw list here;
            # keep CJ's modular model byte-identical instead of round-tripping it.
            output_path = args.output / expanded_path.name
            output_path.write_bytes(source_path.read_bytes())
            count += 1
            continue
        else:
            file_reports = manifest["files"].get(expanded_path.name.casefold())
            if file_reports is None:
                raise ValueError(f"{expanded_path.name}: missing hand manifest")
            if len(expanded.clumps) != len(native.clumps):
                raise ValueError(f"{expanded_path.name}: clump count mismatch")
            report_index = 0
            for expanded_clump, native_clump in zip(expanded.clumps, native.clumps):
                for geometry_index, geometry in enumerate(expanded_clump.geometry_list):
                    report = file_reports[report_index]
                    report_index += 1
                    if report["geometry"] != geometry_index:
                        raise ValueError(f"{expanded_path.name}: geometry manifest mismatch")
                    profile = make_profile(
                        geometry, report, template_indices, templates
                    )
                    previous = profiles_by_hash.get(profile.geometry_hash)
                    if previous is not None and previous != profile:
                        raise ValueError(
                            f"{expanded_path.name}: geometry hash collision"
                        )
                    profiles_by_hash[profile.geometry_hash] = profile
                restore_native_skeleton(expanded_clump, native_clump)
            if report_index != len(file_reports):
                raise ValueError(f"{expanded_path.name}: unused geometry manifest entries")

        output_path = args.output / expanded_path.name
        expanded.write_file(str(output_path), expanded.rw_version)
        validate_runtime_file(output_path, dff)
        count += 1

    txd_paths = [] if args.one else sorted(args.expanded.glob("*.txd"))
    for txd_path in txd_paths:
        (args.output / txd_path.name).write_bytes(txd_path.read_bytes())

    profiles = tuple(profiles_by_hash.values())
    write_runtime_data(args.data, templates, hand_animations, profiles)
    print(
        f"Runtime DFF={count} profiles={len(profiles)} "
        f"blendshape_templates={len(templates)} "
        f"hand_sequences={len(hand_animations)} "
        f"data={args.data} ({args.data.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
