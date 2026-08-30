#!/usr/bin/env python3
"""Export the Handies hand templates and their runtime poses as GLB shape keys.

Run this script through Blender, for example:
    blender --background --python tools/export_blendshape_glb.py -- --output file.glb
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector


FINGER_IDS = tuple(range(3, 18))


def parse_args() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--project",
        type=Path,
        default=Path(r"C:\Users\Digon\Documents\ChatGPT\SanHands2.0"),
    )
    parser.add_argument(
        "--hands",
        type=Path,
        default=Path(r"C:\Users\Digon\Desktop\hands"),
    )
    parser.add_argument(
        "--pose-source",
        type=Path,
        default=Path(r"C:\Users\Digon\Documents\ChatGPT\SanHands\dist\handpose.ifp"),
    )
    parser.add_argument(
        "--gang-source",
        type=Path,
        default=Path(
            r"C:\juegos\Grand Theft Auto San Andreas\modloader\hands\ghands.ifp"
        ),
    )
    parser.add_argument(
        "--dragonff",
        type=Path,
        default=Path(
            r"C:\Users\Digon\AppData\Roaming\Blender Foundation\Blender\5.2"
            r"\extensions\user_default\dragonff"
        ),
    )
    parser.add_argument(
        "--rwfury-root",
        type=Path,
        default=Path(r"C:\Users\Digon\Documents\Fuentes\rwfury-master"),
    )
    parser.add_argument("--signal-time", type=float, default=1.0)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--blend-output", type=Path)
    return parser.parse_args(arguments)


def configure_imports(args: argparse.Namespace) -> None:
    os.environ["DRAGONFF_PATH"] = str(args.dragonff.resolve())
    for path in (args.project, args.dragonff, args.rwfury_root):
        resolved = str(path.resolve())
        if resolved not in sys.path:
            sys.path.insert(0, resolved)


def transpose(matrix):
    return [[matrix[column][row] for column in range(4)] for row in range(4)]


def parent_ids(template) -> dict[int, int]:
    clump = template["clump"]
    frame_to_id = {
        frame_index: frame.bone_data.header.id
        for frame_index, frame in enumerate(clump.frame_list)
        if frame.bone_data is not None
    }
    result = {}
    for bone_id, frame_index in template["id_to_frame"].items():
        parent_frame = clump.frame_list[frame_index].parent
        if parent_frame in frame_to_id:
            result[bone_id] = frame_to_id[parent_frame]
    return result


def deform_vertices(template, rotations) -> list[tuple[float, float, float]]:
    skin = template["skin"]
    geometry = template["geo"]
    index_to_id = template["index_to_id"]
    clump = template["clump"]
    parents = parent_ids(template)

    inverse_bind = [Matrix(transpose(matrix)) for matrix in skin.bone_matrices]
    bind_global = [matrix.inverted_safe() for matrix in inverse_bind]
    pose_by_id = {
        index_to_id[index]: bind_global[index].copy()
        for index in range(min(2, len(bind_global)))
    }

    rotation_by_id = dict(zip(FINGER_IDS, rotations))
    for bone_id in FINGER_IDS:
        frame = clump.frame_list[template["id_to_frame"][bone_id]]
        x, y, z, w = rotation_by_id[bone_id]
        local = Quaternion((w, x, y, z)).normalized().to_matrix().to_4x4()
        local.translation = Vector((frame.position.x, frame.position.y, frame.position.z))
        parent = parents[bone_id]
        pose_by_id[bone_id] = pose_by_id[parent] @ local

    skin_matrices = [
        pose_by_id[index_to_id[index]] @ inverse_bind[index]
        for index in range(len(inverse_bind))
    ]
    result = []
    for vertex, bone_indices, bone_weights in zip(
        geometry.vertices,
        skin.vertex_bone_indices,
        skin.vertex_bone_weights,
    ):
        source = Vector((vertex.x, vertex.y, vertex.z, 1.0))
        output = Vector((0.0, 0.0, 0.0, 0.0))
        for bone_index, weight in zip(bone_indices, bone_weights):
            if weight > 1.0e-8:
                output += (skin_matrices[bone_index] @ source) * weight
        result.append((output.x, output.y, output.z))
    return result


def sample_keys(keys, time, interpolate_quaternion):
    if time <= keys[0][0]:
        return keys[0][1]
    if time >= keys[-1][0]:
        return keys[-1][1]
    for first, second in zip(keys, keys[1:]):
        if first[0] <= time <= second[0]:
            span = second[0] - first[0]
            amount = 0.0 if span <= 1.0e-12 else (time - first[0]) / span
            return interpolate_quaternion(first[1], second[1], amount)
    raise AssertionError("unreachable animation sample")


def calculate_normals(vertices, faces):
    normals = [Vector((0.0, 0.0, 0.0)) for _ in vertices]
    vectors = [Vector(vertex) for vertex in vertices]
    for a, b, c in faces:
        face_normal = (vectors[b] - vectors[a]).cross(vectors[c] - vectors[a])
        if face_normal.length_squared > 1.0e-20:
            normals[a] += face_normal
            normals[b] += face_normal
            normals[c] += face_normal
    result = []
    for normal in normals:
        if normal.length_squared > 1.0e-20:
            normal.normalize()
        else:
            normal = Vector((0.0, 0.0, 1.0))
        result.append(tuple(normal))
    return result


def create_hand_object(name, template, relaxed, targets):
    geometry = template["geo"]
    faces = [(triangle.a, triangle.b, triangle.c) for triangle in geometry.triangles]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(relaxed, [], faces)
    mesh.update(calc_edges=True)
    for polygon in mesh.polygons:
        polygon.use_smooth = True

    normals = calculate_normals(relaxed, faces)
    mesh.normals_split_custom_set_from_vertices(normals)

    if geometry.uv_layers:
        source_uv = geometry.uv_layers[0]
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for polygon in mesh.polygons:
            for loop_index in polygon.loop_indices:
                vertex_index = mesh.loops[loop_index].vertex_index
                uv = source_uv[vertex_index]
                uv_layer.data[loop_index].uv = (uv.u, 1.0 - uv.v)

    material = bpy.data.materials.new(f"{name}_Skin")
    material.diffuse_color = (0.55, 0.28, 0.17, 1.0)
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    if principled is not None:
        principled.inputs["Base Color"].default_value = material.diffuse_color
        principled.inputs["Roughness"].default_value = 0.65
    mesh.materials.append(material)

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj["handies_profile_version"] = 1
    obj["basis_profile"] = "Relaxed"
    obj["coordinates"] = "Original GTA hand-template local space"

    basis = obj.shape_key_add(name="Relaxed")
    basis.interpolation = "KEY_LINEAR"
    for target_name, positions in targets:
        key = obj.shape_key_add(name=target_name)
        key.interpolation = "KEY_LINEAR"
        key.value = 0.0
        for vertex, position in zip(key.data, positions):
            vertex.co = position
    return obj


def clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for datablocks in (bpy.data.meshes, bpy.data.materials, bpy.data.curves):
        for datablock in list(datablocks):
            if datablock.users == 0:
                datablocks.remove(datablock)


def main() -> int:
    args = parse_args()
    configure_imports(args)
    from add_hands import load_template
    from tools.finalize_runtime_models import (
        interpolate_quaternion,
        load_hand_animation_table,
        load_pose_table,
    )

    clear_scene()
    pose_table = load_pose_table(args.pose_source, args.rwfury_root)
    hand_animations = load_hand_animation_table(args.gang_source, args.rwfury_root)
    animation_by_name = {name.casefold(): (duration, tracks) for name, duration, tracks in hand_animations}

    exported = []
    for variant, variant_name in (("s", "Slim"), ("f", "Fat")):
        for side_index, side in enumerate(("L", "R")):
            source_path = args.hands / f"{variant}hand{side.lower()}.dff"
            template = load_template(source_path)
            relaxed = deform_vertices(template, pose_table[side_index][0])
            targets = [
                ("Grip", deform_vertices(template, pose_table[side_index][1])),
            ]
            if side == "R":
                targets.append(
                    ("FuckU", deform_vertices(template, pose_table[side_index][2]))
                )
            for signal_index in range(1, 6):
                animation_name = f"{side}HGsign{signal_index}"
                _, tracks = animation_by_name[animation_name.casefold()]
                rotations = tuple(
                    sample_keys(track, args.signal_time, interpolate_quaternion)
                    for track in tracks
                )
                targets.append(
                    (animation_name, deform_vertices(template, rotations))
                )
            object_name = f"{variant_name}_{'Left' if side == 'L' else 'Right'}"
            obj = create_hand_object(object_name, template, relaxed, targets)
            obj["source_dff"] = source_path.name
            obj["signal_sample_time"] = args.signal_time
            exported.append(obj)

    for obj in bpy.context.selected_objects:
        obj.select_set(False)
    for obj in exported:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = exported[0]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(args.output),
        export_format="GLB",
        use_selection=True,
        export_normals=False,
        export_morph=True,
        export_morph_normal=False,
        export_morph_tangent=False,
        export_attributes=True,
        export_extras=True,
        export_yup=True,
    )
    if args.blend_output is not None:
        args.blend_output.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend_output))

    print(
        f"HANDIES_GLTF objects={len(exported)} "
        f"profiles={sum(len(obj.data.shape_keys.key_blocks) for obj in exported)} "
        f"output={args.output} size={args.output.stat().st_size}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
