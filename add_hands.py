from __future__ import annotations

import argparse
import copy
import math
import shutil
from collections import Counter
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()

from gtaLib.dff import (  # noqa: E402
    Bone,
    HAnimHeader,
    RGBA,
    Sphere,
    TexCoords,
    Triangle,
    Vector,
    dff,
)


SIDES = {
    "L": {"forearm": 33, "hand": 34, "finger": 35, "finger1": 36, "id_base": 1000},
    "R": {"forearm": 23, "hand": 24, "finger": 25, "finger1": 26, "id_base": 1100},
}


def transpose(matrix):
    return [[matrix[j][i] for j in range(4)] for i in range(4)]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_vec(matrix, value, w=1.0):
    source = (value.x, value.y, value.z, w)
    result = [sum(matrix[i][j] * source[j] for j in range(4)) for i in range(4)]
    return Vector(result[0], result[1], result[2])


def inverse(matrix):
    augmented = [row[:] + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(matrix)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda row: abs(augmented[row][col]))
        if abs(augmented[pivot][col]) < 1e-12:
            raise ValueError("Singular matrix")
        augmented[col], augmented[pivot] = augmented[pivot], augmented[col]
        divisor = augmented[col][col]
        augmented[col] = [value / divisor for value in augmented[col]]
        for row in range(4):
            if row == col:
                continue
            factor = augmented[row][col]
            augmented[row] = [
                augmented[row][i] - factor * augmented[col][i] for i in range(8)
            ]
    return [row[4:] for row in augmented]


def scale_matrix(value):
    return [
        [value, 0.0, 0.0, 0.0],
        [0.0, value, 0.0, 0.0],
        [0.0, 0.0, value, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]


def global_from_skin(stored_matrix):
    return inverse(transpose(stored_matrix))


def stored_from_global(global_matrix):
    return transpose(inverse(global_matrix))


def vector_length(vector):
    return math.sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z)


def normalize(vector):
    length = vector_length(vector)
    if length < 1e-12:
        return vector
    return Vector(vector.x / length, vector.y / length, vector.z / length)


def dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z


def sub(a, b):
    return Vector(a.x - b.x, a.y - b.y, a.z - b.z)


def add_scaled(origin, a, sa, b, sb):
    return Vector(origin.x + a.x * sa + b.x * sb, origin.y + a.y * sa + b.y * sb, origin.z + a.z * sa + b.z * sb)


def distance_sq(a, b):
    d = sub(a, b)
    return dot(d, d)


def closest_barycentric(point, a, b, c):
    # Closest point on a triangle, returning weights for a, b and c.
    ab = sub(b, a)
    ac = sub(c, a)
    ap = sub(point, a)
    d1, d2 = dot(ab, ap), dot(ac, ap)
    if d1 <= 0.0 and d2 <= 0.0:
        return (1.0, 0.0, 0.0), distance_sq(point, a)

    bp = sub(point, b)
    d3, d4 = dot(ab, bp), dot(ac, bp)
    if d3 >= 0.0 and d4 <= d3:
        return (0.0, 1.0, 0.0), distance_sq(point, b)

    vc = d1 * d4 - d3 * d2
    if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
        v = d1 / (d1 - d3)
        closest = add_scaled(a, ab, v, ac, 0.0)
        return (1.0 - v, v, 0.0), distance_sq(point, closest)

    cp = sub(point, c)
    d5, d6 = dot(ab, cp), dot(ac, cp)
    if d6 >= 0.0 and d5 <= d6:
        return (0.0, 0.0, 1.0), distance_sq(point, c)

    vb = d5 * d2 - d1 * d6
    if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
        w = d2 / (d2 - d6)
        closest = add_scaled(a, ab, 0.0, ac, w)
        return (1.0 - w, 0.0, w), distance_sq(point, closest)

    va = d3 * d6 - d5 * d4
    if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
        edge = sub(c, b)
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        closest = Vector(b.x + edge.x * w, b.y + edge.y * w, b.z + edge.z * w)
        return (0.0, 1.0 - w, w), distance_sq(point, closest)

    denom = 1.0 / (va + vb + vc)
    v, w = vb * denom, vc * denom
    closest = add_scaled(a, ab, v, ac, w)
    return (1.0 - v - w, v, w), distance_sq(point, closest)


def interpolate(values, indices, weights, ctor):
    result = []
    for field in range(len(values[indices[0]])):
        result.append(sum(values[index][field] * weight for index, weight in zip(indices, weights)))
    return ctor(*result)


def interpolate_color(values, indices, weights):
    values_out = []
    for field in range(4):
        value = sum(values[index][field] * weight for index, weight in zip(indices, weights))
        values_out.append(max(0, min(255, round(value))))
    return RGBA(*values_out)


def recompute_sphere(vertices):
    if not vertices:
        return Sphere(0.0, 0.0, 0.0, 0.0)
    center = Vector(
        (min(v.x for v in vertices) + max(v.x for v in vertices)) * 0.5,
        (min(v.y for v in vertices) + max(v.y for v in vertices)) * 0.5,
        (min(v.z for v in vertices) + max(v.z for v in vertices)) * 0.5,
    )
    radius = math.sqrt(max(distance_sq(center, v) for v in vertices))
    return Sphere(center.x, center.y, center.z, radius)


def descendants(frames, root_index):
    result = set()
    for index in range(len(frames)):
        current = index
        visited = set()
        while current >= 0 and current not in visited:
            if current == root_index:
                result.add(index)
                break
            visited.add(current)
            current = frames[current].parent
    return result


def load_template(path):
    model = dff()
    model.load_file(str(path))
    clump = model.clumps[0]
    geo = clump.geometry_list[0]
    root_index = next(i for i, frame in enumerate(clump.frame_list) if frame.bone_data and frame.bone_data.bones)
    id_to_frame = {
        frame.bone_data.header.id: i
        for i, frame in enumerate(clump.frame_list)
        if frame.bone_data is not None
    }
    root = clump.frame_list[root_index]
    id_to_bone = {bone.id: bone for bone in root.bone_data.bones}
    index_to_id = {bone.index: bone.id for bone in root.bone_data.bones}
    return {
        "model": model,
        "clump": clump,
        "geo": geo,
        "skin": geo.extensions["skin"],
        "root": root,
        "id_to_frame": id_to_frame,
        "id_to_bone": id_to_bone,
        "index_to_id": index_to_id,
        "hand_length": vector_length(clump.frame_list[id_to_frame[2]].position),
    }


def choose_template(templates, side, target_length):
    candidates = [templates[(variant, side)] for variant in ("f", "s")]
    return min(candidates, key=lambda item: abs(item["hand_length"] - target_length))


def skeleton_for_geometry(clump, geometry_index):
    atomic = next((item for item in clump.atomic_list if item.geometry == geometry_index), None)
    if atomic is None:
        raise ValueError(f"No atomic for geometry {geometry_index}")
    roots = []
    current = atomic.frame
    visited = set()
    while current >= 0 and current not in visited:
        visited.add(current)
        frame = clump.frame_list[current]
        if frame.bone_data and frame.bone_data.bones:
            roots = [current]
            break
        current = frame.parent
    if not roots:
        candidates = descendants(clump.frame_list, atomic.frame)
        roots = [
            index
            for index in candidates
            if clump.frame_list[index].bone_data and clump.frame_list[index].bone_data.bones
        ]
    if len(roots) != 1:
        raise ValueError(f"Expected one HAnim root for geometry {geometry_index}, got {roots}")
    root_index = roots[0]
    skeleton_indices = descendants(clump.frame_list, root_index)
    id_to_frame = {
        clump.frame_list[index].bone_data.header.id: index
        for index in skeleton_indices
        if clump.frame_list[index].bone_data is not None
    }
    return root_index, id_to_frame


def nearest_surface_data(point, removed_triangles, vertices):
    best = None
    best_dist = float("inf")
    for tri in removed_triangles:
        indices = (tri.a, tri.b, tri.c)
        weights, dist = closest_barycentric(point, *(vertices[index] for index in indices))
        if dist < best_dist:
            best_dist = dist
            best = (tri, indices, weights)
    if best is None:
        raise ValueError("No original hand surface available for UV transfer")
    return best


def make_side_plan(
    clump,
    geo,
    skin,
    root,
    id_to_frame,
    template,
    side,
    used_ids,
    next_skin_index,
    next_frame_index,
):
    config = SIDES[side]
    id_to_bone = {bone.id: bone for bone in root.bone_data.bones}
    required = (config["forearm"], config["hand"], config["finger"], config["finger1"])
    if not all(bone_id in id_to_bone and bone_id in id_to_frame for bone_id in required):
        raise ValueError(f"Missing standard {side} hand bones")

    target_length = vector_length(clump.frame_list[id_to_frame[config["hand"]]].position)
    ratio = target_length / template["hand_length"]
    target_forearm_index = id_to_bone[config["forearm"]].index
    target_forearm_global = global_from_skin(skin.bone_matrices[target_forearm_index])
    transform = mat_mul(target_forearm_global, scale_matrix(ratio))
    inverse_forearm = inverse(target_forearm_global)

    source_geo = template["geo"]
    cut = min(vertex.x for vertex in source_geo.vertices) * ratio
    arm_indices = {id_to_bone[bone_id].index for bone_id in required}
    cut_vertices = set()
    for index, (vertex, bone_indices, weights) in enumerate(
        zip(geo.vertices, skin.vertex_bone_indices, skin.vertex_bone_weights)
    ):
        arm_weight = sum(weight for bone_index, weight in zip(bone_indices, weights) if bone_index in arm_indices)
        local = mat_vec(inverse_forearm, vertex)
        if arm_weight > 0.15 and local.x >= cut - 0.004:
            cut_vertices.add(index)

    removed = [
        tri for tri in geo.triangles if cut_vertices.intersection((tri.a, tri.b, tri.c))
    ]
    if len(removed) < 8:
        raise ValueError(f"Only {len(removed)} {side} hand triangles selected")

    source_to_target_id = {
        1: config["forearm"],
        2: config["hand"],
        3: config["finger"],
        4: config["finger1"],
    }
    for source_id in range(5, 18):
        candidate = config["id_base"] + source_id
        while candidate in used_ids:
            candidate += 100
        used_ids.add(candidate)
        source_to_target_id[source_id] = candidate

    source_to_target_index = {}
    for source_id in range(1, 5):
        source_to_target_index[source_id] = id_to_bone[source_to_target_id[source_id]].index

    new_bones = []
    new_frames = []
    for source_id in range(5, 18):
        source_bone = template["id_to_bone"][source_id]
        target_id = source_to_target_id[source_id]
        source_to_target_index[source_id] = next_skin_index
        new_bones.append(Bone(target_id, next_skin_index, source_bone.type))
        next_skin_index += 1

    # Source fingers 3 and 4 reuse the two standard GTA finger frames.
    source_frame_to_target_frame = {
        template["id_to_frame"][1]: id_to_frame[config["forearm"]],
        template["id_to_frame"][2]: id_to_frame[config["hand"]],
        template["id_to_frame"][3]: id_to_frame[config["finger"]],
        template["id_to_frame"][4]: id_to_frame[config["finger1"]],
    }
    for source_id in (3, 4):
        source_frame = template["clump"].frame_list[template["id_to_frame"][source_id]]
        target_frame = clump.frame_list[source_frame_to_target_frame[template["id_to_frame"][source_id]]]
        target_id = source_to_target_id[source_id]
        target_frame.rotation_matrix = copy.deepcopy(source_frame.rotation_matrix)
        target_frame.position = Vector(
            source_frame.position.x * ratio,
            source_frame.position.y * ratio,
            source_frame.position.z * ratio,
        )
        target_frame.bone_data.header = HAnimHeader(
            target_frame.bone_data.header.version, target_id, 0
        )

    # Append the other thirteen frames, preserving the native finger hierarchy.
    for source_id in range(5, 18):
        source_index = template["id_to_frame"][source_id]
        source_frame = template["clump"].frame_list[source_index]
        target_frame = copy.deepcopy(source_frame)
        target_frame.position = Vector(
            source_frame.position.x * ratio,
            source_frame.position.y * ratio,
            source_frame.position.z * ratio,
        )
        target_frame.bone_data.header = HAnimHeader(
            target_frame.bone_data.header.version, source_to_target_id[source_id], 0
        )
        target_index = next_frame_index + len(new_frames)
        source_frame_to_target_frame[source_index] = target_index
        parent_source = source_frame.parent
        if parent_source not in source_frame_to_target_frame:
            raise ValueError(f"Unresolved source parent for {side} bone {source_id}")
        target_frame.parent = source_frame_to_target_frame[parent_source]
        new_frames.append(target_frame)

    replacement_bones = []
    for source_id in range(3, 18):
        source_bone = template["id_to_bone"][source_id]
        replacement_bones.append(
            Bone(
                source_to_target_id[source_id],
                source_to_target_index[source_id],
                source_bone.type,
            )
        )

    # Replace bind matrices for reused finger bones and append matrices for new bones.
    matrix_updates = {}
    for source_id in range(3, 18):
        source_index = template["id_to_bone"][source_id].index
        source_global = global_from_skin(template["skin"].bone_matrices[source_index])
        target_global = mat_mul(transform, source_global)
        matrix_updates[source_to_target_index[source_id]] = stored_from_global(target_global)

    skin_index_map = {}
    for source_index, source_id in template["index_to_id"].items():
        skin_index_map[source_index] = source_to_target_index[source_id]

    return {
        "side": side,
        "template": template,
        "ratio": ratio,
        "transform": transform,
        "removed": removed,
        "replacement_bones": replacement_bones,
        "replace_start_id": config["finger"],
        "replace_skip_id": config["finger1"],
        "new_bones": new_bones,
        "new_frames": new_frames,
        "matrix_updates": matrix_updates,
        "skin_index_map": skin_index_map,
    }


def rebuild_geometry(geo, skin, plans):
    old_vertices = list(geo.vertices)
    old_normals = list(geo.normals)
    old_uv_layers = [list(layer) for layer in geo.uv_layers]
    old_prelit = list(geo.prelit_colors)
    old_indices = list(skin.vertex_bone_indices)
    old_weights = list(skin.vertex_bone_weights)
    old_triangles = list(geo.triangles)
    extra = geo.extensions.get("extra_vert_color")
    old_extra = list(extra.colors) if extra is not None else []

    removed_set = {id(tri) for plan in plans for tri in plan["removed"]}
    kept_triangles = [tri for tri in old_triangles if id(tri) not in removed_set]
    used_old = sorted({index for tri in kept_triangles for index in (tri.a, tri.b, tri.c)})
    remap = {old: new for new, old in enumerate(used_old)}

    geo.vertices = [old_vertices[index] for index in used_old]
    geo.normals = [old_normals[index] for index in used_old] if old_normals else []
    geo.uv_layers = [[layer[index] for index in used_old] for layer in old_uv_layers]
    geo.prelit_colors = [old_prelit[index] for index in used_old] if old_prelit else []
    skin.vertex_bone_indices = [old_indices[index] for index in used_old]
    skin.vertex_bone_weights = [old_weights[index] for index in used_old]
    if extra is not None:
        extra.colors = [old_extra[index] for index in used_old]

    geo.triangles = [
        tri._replace(a=remap[tri.a], b=remap[tri.b], c=remap[tri.c]) for tri in kept_triangles
    ]

    for plan in plans:
        source_geo = plan["template"]["geo"]
        source_skin = plan["template"]["skin"]
        base = len(geo.vertices)
        nearest = []
        for source_vertex in source_geo.vertices:
            transformed = mat_vec(plan["transform"], source_vertex)
            geo.vertices.append(transformed)
            nearest.append(nearest_surface_data(transformed, plan["removed"], old_vertices))

        if old_normals:
            for source_normal in source_geo.normals:
                geo.normals.append(normalize(mat_vec(plan["transform"], source_normal, 0.0)))

        for layer_index, old_layer in enumerate(old_uv_layers):
            for _, indices, weights in nearest:
                geo.uv_layers[layer_index].append(interpolate(old_layer, indices, weights, TexCoords))

        if old_prelit:
            for _, indices, weights in nearest:
                geo.prelit_colors.append(interpolate_color(old_prelit, indices, weights))

        if extra is not None:
            for _, indices, weights in nearest:
                extra.colors.append(interpolate_color(old_extra, indices, weights))

        for source_indices, source_weights in zip(
            source_skin.vertex_bone_indices, source_skin.vertex_bone_weights
        ):
            skin.vertex_bone_indices.append(
                tuple(plan["skin_index_map"][index] for index in source_indices)
            )
            skin.vertex_bone_weights.append(tuple(source_weights))

        vertex_materials = [item[0].material for item in nearest]
        for source_tri in source_geo.triangles:
            material = Counter(
                vertex_materials[index] for index in (source_tri.a, source_tri.b, source_tri.c)
            ).most_common(1)[0][0]
            geo.triangles.append(
                Triangle(
                    source_tri.b + base,
                    source_tri.a + base,
                    material,
                    source_tri.c + base,
                )
            )

    geo.bounding_sphere = recompute_sphere(geo.vertices)
    geo.extensions.pop("mat_split", None)


def process_geometry(clump, geometry_index, templates):
    geo = clump.geometry_list[geometry_index]
    skin = geo.extensions.get("skin")
    if skin is None:
        raise ValueError(f"Geometry {geometry_index} has no skin")
    root_index, id_to_frame = skeleton_for_geometry(clump, geometry_index)
    root = clump.frame_list[root_index]
    used_ids = {bone.id for bone in root.bone_data.bones}

    plans = []
    for side in ("L", "R"):
        hand_frame = clump.frame_list[id_to_frame[SIDES[side]["hand"]]]
        template = choose_template(templates, side, vector_length(hand_frame.position))
        plans.append(
            make_side_plan(
                clump,
                geo,
                skin,
                root,
                id_to_frame,
                template,
                side,
                used_ids,
                skin.num_bones + 13 * len(plans),
                len(clump.frame_list) + 13 * len(plans),
            )
        )

    rebuild_geometry(geo, skin, plans)

    for plan in plans:
        clump.frame_list.extend(plan["new_frames"])
        for index, matrix in plan["matrix_updates"].items():
            if index < len(skin.bone_matrices):
                skin.bone_matrices[index] = matrix
            elif index == len(skin.bone_matrices):
                skin.bone_matrices.append(matrix)
            else:
                raise ValueError(f"Non-contiguous skin matrix index {index}")

    replacements = {plan["replace_start_id"]: plan for plan in plans}
    skip_ids = {plan["replace_skip_id"] for plan in plans}
    new_root_bones = []
    for bone in root.bone_data.bones:
        if bone.id in replacements:
            new_root_bones.extend(replacements[bone.id]["replacement_bones"])
        elif bone.id not in skip_ids:
            new_root_bones.append(bone)
    root.bone_data.bones = new_root_bones
    root.bone_data.header = HAnimHeader(
        root.bone_data.header.version,
        root.bone_data.header.id,
        len(new_root_bones),
    )
    skin.num_bones = len(new_root_bones)

    return {
        "geometry": geometry_index,
        "removed_left": len(plans[0]["removed"]),
        "removed_right": len(plans[1]["removed"]),
        "left_template": "f" if plans[0]["template"]["hand_length"] < 0.25 else "s",
        "right_template": "f" if plans[1]["template"]["hand_length"] < 0.25 else "s",
        "vertices": len(geo.vertices),
        "triangles": len(geo.triangles),
        "bones": skin.num_bones,
        "matrices": len(skin.bone_matrices),
    }


def process_file(input_path, output_path, templates):
    model = dff()
    model.load_file(str(input_path))
    reports = []
    for clump in model.clumps:
        for geometry_index in range(len(clump.geometry_list)):
            reports.append(process_geometry(clump, geometry_index, templates))
    for report in reports:
        if report["bones"] != report["matrices"]:
            raise ValueError(
                f"Pre-write bone/matrix mismatch: {report['bones']} vs {report['matrices']}"
            )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    model.write_file(str(output_path), model.rw_version)
    return reports


def validate_file(path):
    model = dff()
    model.load_file(str(path))
    for clump in model.clumps:
        for index, frame in enumerate(clump.frame_list):
            if frame.parent >= len(clump.frame_list):
                raise ValueError(f"Frame {index} has an out-of-range parent")
        for geo in clump.geometry_list:
            skin = geo.extensions.get("skin")
            if skin is None:
                raise ValueError("Output geometry lacks Skin PLG")
            count = len(geo.vertices)
            if len(skin.vertex_bone_indices) != count or len(skin.vertex_bone_weights) != count:
                raise ValueError("Output skin/vertex count mismatch")
            if geo.normals and len(geo.normals) != count:
                raise ValueError("Output normal/vertex count mismatch")
            if any(len(layer) != count for layer in geo.uv_layers):
                raise ValueError("Output UV/vertex count mismatch")
            if len(skin.bone_matrices) != skin.num_bones:
                raise ValueError("Output skin matrix/bone count mismatch")
            if max((max(indices) for indices in skin.vertex_bone_indices), default=0) >= skin.num_bones:
                raise ValueError("Output uses an out-of-range bone index")
            if any(max(tri.a, tri.b, tri.c) >= count for tri in geo.triangles):
                raise ValueError("Output triangle uses an out-of-range vertex")
            roots = [f for f in clump.frame_list if f.bone_data and f.bone_data.bones]
            if not any(len(root.bone_data.bones) == skin.num_bones for root in roots):
                raise ValueError("No matching HAnim root for output skin")
        for geometry_index in range(len(clump.geometry_list)):
            root_index, _ = skeleton_for_geometry(clump, geometry_index)
            subtree = descendants(clump.frame_list, root_index)
            frame_ids = [
                clump.frame_list[index].bone_data.header.id
                for index in subtree
                if clump.frame_list[index].bone_data is not None
            ]
            root_ids = [bone.id for bone in clump.frame_list[root_index].bone_data.bones]
            if Counter(frame_ids) != Counter(root_ids):
                raise ValueError("HAnim bone IDs do not match their frame subtree")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--hands", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
    parser.add_argument("--one", help="Process only one DFF filename")
    parser.add_argument("--copy-txd", action="store_true")
    args = parser.parse_args()

    templates = {}
    for variant in ("f", "s"):
        for side in ("l", "r"):
            templates[(variant, side.upper())] = load_template(args.hands / f"{variant}hand{side}.dff")

    paths = [args.input / args.one] if args.one else sorted(args.input.glob("*.dff"))
    failures = []
    unchanged = []
    for index, input_path in enumerate(paths, 1):
        try:
            output_path = args.output / input_path.name
            if input_path.name.casefold() == "player.dff":
                output_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(input_path, output_path)
                validate_file(output_path)
                unchanged.append(input_path.name)
                print(
                    f"UNCHANGED {index}/{len(paths)} {input_path.name}: "
                    "modular CJ skeleton has no hand surface"
                )
                continue
            reports = process_file(input_path, output_path, templates)
            validate_file(output_path)
            details = "; ".join(
                f"g{r['geometry']} rm={r['removed_left']}/{r['removed_right']} "
                f"tpl={r['left_template']}/{r['right_template']} bones={r['bones']}"
                for r in reports
            )
            print(f"OK {index}/{len(paths)} {input_path.name}: {details}")
        except Exception as exc:
            failures.append((input_path.name, str(exc)))
            print(f"FAIL {index}/{len(paths)} {input_path.name}: {exc}")

    if args.copy_txd and not args.one:
        for path in args.input.glob("*.txd"):
            shutil.copy2(path, args.output / path.name)

    modified = len(paths) - len(failures) - len(unchanged)
    print(
        f"SUMMARY dff={len(paths)} modified={modified} "
        f"unchanged={len(unchanged)} failed={len(failures)}"
    )
    for name, message in failures:
        print(f"ERROR {name}: {message}")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
