#!/usr/bin/env python3
"""Extract an edited GLB into Handies' topology-stable morph template data."""

from __future__ import annotations

import argparse
import math
import os
import sys
from pathlib import Path

import bpy
from mathutils import Vector


EXPECTED = {
    "Slim_Left": "shandl.dff",
    "Slim_Right": "shandr.dff",
    "Fat_Left": "fhandl.dff",
    "Fat_Right": "fhandr.dff",
}


def parse_args() -> argparse.Namespace:
    values = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--hands", required=True, type=Path)
    parser.add_argument("--pose-source", required=True, type=Path)
    parser.add_argument("--dragonff", required=True, type=Path)
    parser.add_argument("--rwfury-root", required=True, type=Path)
    return parser.parse_args(values)


def configure_imports(args: argparse.Namespace) -> None:
    os.environ["DRAGONFF_PATH"] = str(args.dragonff.resolve())
    for path in (args.project, args.dragonff, args.rwfury_root):
        value = str(path.resolve())
        if value not in sys.path:
            sys.path.insert(0, value)


def vertex_uvs(mesh) -> list[tuple[float, float]]:
    result = [None] * len(mesh.vertices)
    if not mesh.uv_layers:
        raise ValueError(f"{mesh.name}: missing UV layer")
    layer = mesh.uv_layers.active.data
    for loop in mesh.loops:
        uv = layer[loop.index].uv
        current = result[loop.vertex_index]
        value = (float(uv.x), float(uv.y))
        if current is None:
            result[loop.vertex_index] = value
    if any(value is None for value in result):
        raise ValueError(f"{mesh.name}: vertex without UV")
    return result


def calculate_normals(vertices, faces):
    vectors = [Vector(value) for value in vertices]
    normals = [Vector((0.0, 0.0, 0.0)) for _ in vertices]
    for a, b, c in faces:
        normal = (vectors[b] - vectors[a]).cross(vectors[c] - vectors[a])
        if normal.length_squared <= 1.0e-20:
            continue
        normals[a] += normal
        normals[b] += normal
        normals[c] += normal
    output = []
    for normal in normals:
        if normal.length_squared > 1.0e-20:
            normal.normalize()
        else:
            normal = Vector((0.0, 0.0, 1.0))
        output.append((normal.x, normal.y, normal.z))
    return tuple(output)


def reference_data(args, object_name, load_template, load_pose_table, deform_vertices):
    template = load_template(args.hands / EXPECTED[object_name])
    side_index = 0 if object_name.endswith("_Left") else 1
    poses = load_pose_table(args.pose_source, args.rwfury_root)
    positions = deform_vertices(template, poses[side_index][0])
    source_uv = template["geo"].uv_layers[0]
    uvs = [(float(value.u), 1.0 - float(value.v)) for value in source_uv]
    faces = [
        (triangle.a, triangle.b, triangle.c)
        for triangle in template["geo"].triangles
    ]
    return template, positions, uvs, faces


def build_mapping(obj, reference_positions, reference_uvs):
    mesh = obj.data
    edited_uvs = vertex_uvs(mesh)
    basis = mesh.shape_keys.key_blocks[0]
    groups = [[] for _ in reference_positions]
    maximum_reference_distance = 0.0
    for edited_index, (vertex, uv) in enumerate(zip(basis.data, edited_uvs)):
        world = obj.matrix_world @ vertex.co
        uv_candidates = [
            index
            for index, reference_uv in enumerate(reference_uvs)
            if (uv[0] - reference_uv[0]) ** 2 + (uv[1] - reference_uv[1]) ** 2
            <= 1.0e-8
        ]
        candidates = uv_candidates or range(len(reference_positions))
        reference_index = min(
            candidates,
            key=lambda index: (
                world.x - reference_positions[index][0]
            ) ** 2
            + (world.y - reference_positions[index][1]) ** 2
            + (world.z - reference_positions[index][2]) ** 2,
        )
        reference = reference_positions[reference_index]
        distance = math.sqrt(
            (world.x - reference[0]) ** 2
            + (world.y - reference[1]) ** 2
            + (world.z - reference[2]) ** 2
        )
        maximum_reference_distance = max(maximum_reference_distance, distance)
        groups[reference_index].append(edited_index)
    missing = [index for index, group in enumerate(groups) if not group]
    if missing:
        raise ValueError(f"{obj.name}: {len(missing)} source vertices are unmapped")
    return groups, maximum_reference_distance


def collapse_key(obj, key, groups):
    result = []
    maximum_split = 0.0
    for group in groups:
        values = [obj.matrix_world @ key.data[index].co for index in group]
        center = sum(values, Vector((0.0, 0.0, 0.0))) / len(values)
        maximum_split = max(
            maximum_split,
            max((value - center).length for value in values),
        )
        result.append((center.x, center.y, center.z))
    return tuple(result), maximum_split


def main() -> int:
    args = parse_args()
    configure_imports(args)
    from add_hands import load_template
    from tools.blendshape_profiles import MorphTarget, MorphTemplate, write_profiles
    from tools.export_blendshape_glb import deform_vertices
    from tools.finalize_runtime_models import load_pose_table

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.import_scene.gltf(filepath=str(args.input))

    templates = []
    for object_name in EXPECTED:
        obj = bpy.data.objects.get(object_name)
        if obj is None or obj.type != "MESH" or obj.data.shape_keys is None:
            raise ValueError(f"Missing blendshape mesh {object_name}")
        template, reference_positions, reference_uvs, faces = reference_data(
            args, object_name, load_template, load_pose_table, deform_vertices
        )
        groups, reference_distance = build_mapping(
            obj, reference_positions, reference_uvs
        )
        targets = []
        maximum_split = 0.0
        for key_index, key in enumerate(obj.data.shape_keys.key_blocks):
            positions, split = collapse_key(obj, key, groups)
            maximum_split = max(maximum_split, split)
            target_name = "Relaxed" if key_index == 0 else key.name
            normals = calculate_normals(positions, faces)
            targets.append(MorphTarget(target_name, positions, normals))
        templates.append(MorphTemplate(object_name, tuple(targets)))
        print(
            f"BLENDSHAPE_EXTRACT {object_name} glb_vertices={len(obj.data.vertices)} "
            f"source_vertices={len(template['geo'].vertices)} "
            f"basis_distance={reference_distance:.8f} split={maximum_split:.8f} "
            f"targets={len(targets)}"
        )
    write_profiles(args.output, tuple(templates))
    print(f"BLENDSHAPE_DATA output={args.output} size={args.output.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
