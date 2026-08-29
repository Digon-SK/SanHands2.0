#!/usr/bin/env python3
"""Build native 32-bone ped DFFs plus the runtime finger data for Handies."""

from __future__ import annotations

import argparse
import copy
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"HND2DAT\0"
VERSION = 2
NATIVE_BONES = 32
RUNTIME_BONES = 58
FINGER_IDS = tuple(range(3, 18))
POSE_TIMES = (0.56, 0.6666667, 1.3333333)


@dataclass(frozen=True)
class RuntimeProfile:
    geometry_hash: int
    vertex_count: int
    translations: tuple[tuple[float, float, float], ...]
    indices: tuple[tuple[int, int, int, int], ...]
    weights: tuple[tuple[float, float, float, float], ...]
    matrices: tuple[tuple[tuple[float, float, float, float], ...], ...]


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


def load_hand_signal_table(gang_source: Path, rwfury_root: Path):
    """Load the original left/right gang-sign finger tracks without the wrists."""
    sys.path.insert(0, str(rwfury_root.resolve()))
    from rwfury import Ifp

    package = Ifp.from_bytes(gang_source.read_bytes())
    if package.internal_name.upper() != "GHANDS":
        raise ValueError(f"{gang_source}: expected the GHANDS animation package")

    result = []
    for side in ("L", "R"):
        side_animations = []
        for signal_index in range(1, 6):
            animation_name = f"{side}HGsign{signal_index}"
            animation = package.get_animation(animation_name)
            if animation is None:
                raise ValueError(f"Missing animation {animation_name}")
            by_id = {obj.bone_id: obj for obj in animation.objects}
            if not all(bone_id in by_id for bone_id in FINGER_IDS):
                raise ValueError(f"{animation_name}: incomplete finger tracks")

            tracks = []
            duration = 0.0
            for bone_id in FINGER_IDS:
                frames = sorted(by_id[bone_id].frames, key=lambda frame: frame.time)
                if not frames or len(frames) > 64:
                    raise ValueError(
                        f"{animation_name}: invalid key count for bone {bone_id}"
                    )
                keys = tuple(
                    (float(frame.time), normalize_quaternion(tuple(frame.rotation)))
                    for frame in frames
                )
                duration = max(duration, keys[-1][0])
                tracks.append(keys)
            if duration <= 0.0:
                raise ValueError(f"{animation_name}: invalid duration")
            side_animations.append((duration, tuple(tracks)))
        result.append(tuple(side_animations))
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


def target_id(side: str, source_id: int) -> int:
    if side == "L":
        return {3: 35, 4: 36}.get(source_id, 1000 + source_id)
    return {3: 25, 4: 26}.get(source_id, 1100 + source_id)


def make_profile(geometry, clump, geometry_index) -> RuntimeProfile:
    skin = geometry.extensions.get("skin")
    if skin is None or skin.num_bones != RUNTIME_BONES:
        raise ValueError("Expanded geometry does not have a 58-bone skin")
    root, id_to_frame = skeleton_for_geometry(clump, geometry_index)
    translations = []
    for side in ("L", "R"):
        for source_id in FINGER_IDS:
            frame = id_to_frame[target_id(side, source_id)]
            translations.append((frame.position.x, frame.position.y, frame.position.z))

    if len(root.bone_data.bones) != RUNTIME_BONES:
        raise ValueError("Expanded HAnim root is not 58 bones")
    if len(skin.bone_matrices) != RUNTIME_BONES:
        raise ValueError("Expanded skin matrix count is not 58")
    return RuntimeProfile(
        geometry_hash=fnv1a_vertices(geometry.vertices),
        vertex_count=len(geometry.vertices),
        translations=tuple(translations),
        indices=tuple(tuple(value) for value in skin.vertex_bone_indices),
        weights=tuple(tuple(value) for value in skin.vertex_bone_weights),
        matrices=tuple(
            tuple(tuple(component for component in row) for row in matrix)
            for matrix in skin.bone_matrices
        ),
    )


def collapsed_bone_index(expanded_index: int, expanded_index_to_id, native_id_to_index):
    bone_id = expanded_index_to_id[expanded_index]
    if 1005 <= bone_id <= 1017:
        return native_id_to_index[36 if bone_id == 1005 else 35]
    if 1105 <= bone_id <= 1117:
        return native_id_to_index[26 if bone_id == 1105 else 25]
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


def write_runtime_data(path: Path, poses, hand_signals, profiles):
    data = bytearray(MAGIC)
    data += struct.pack("<II", VERSION, len(profiles))
    for side in poses:
        for pose in side:
            for quaternion in pose:
                data += struct.pack("<4f", *quaternion)
    for side in hand_signals:
        for duration, tracks in side:
            data += struct.pack("<f", duration)
            for track in tracks:
                data += struct.pack("<I", len(track))
                for time, quaternion in track:
                    data += struct.pack("<5f", time, *quaternion)
    for profile in profiles:
        data += struct.pack(
            "<QII", profile.geometry_hash, profile.vertex_count, RUNTIME_BONES
        )
        for translation in profile.translations:
            data += struct.pack("<3f", *translation)
        for indices in profile.indices:
            data += struct.pack("<4B", *indices)
        for weights in profile.weights:
            data += struct.pack("<4f", *weights)
        for matrix in profile.matrices:
            for row in matrix:
                data += struct.pack("<4f", *row)
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

    poses = load_pose_table(args.pose_source, args.rwfury_root)
    hand_signals = load_hand_signal_table(args.gang_source, args.rwfury_root)
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
            if len(expanded.clumps) != len(native.clumps):
                raise ValueError(f"{expanded_path.name}: clump count mismatch")
            for expanded_clump, native_clump in zip(expanded.clumps, native.clumps):
                for geometry_index, geometry in enumerate(expanded_clump.geometry_list):
                    profile = make_profile(geometry, expanded_clump, geometry_index)
                    previous = profiles_by_hash.get(profile.geometry_hash)
                    if previous is not None and previous != profile:
                        raise ValueError(
                            f"{expanded_path.name}: geometry hash collision"
                        )
                    profiles_by_hash[profile.geometry_hash] = profile
                restore_native_skeleton(expanded_clump, native_clump)

        output_path = args.output / expanded_path.name
        expanded.write_file(str(output_path), expanded.rw_version)
        validate_runtime_file(output_path, dff)
        count += 1

    txd_paths = [] if args.one else sorted(args.expanded.glob("*.txd"))
    for txd_path in txd_paths:
        (args.output / txd_path.name).write_bytes(txd_path.read_bytes())

    profiles = tuple(profiles_by_hash.values())
    write_runtime_data(args.data, poses, hand_signals, profiles)
    print(
        f"Runtime DFF={count} profiles={len(profiles)} "
        f"data={args.data} ({args.data.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
