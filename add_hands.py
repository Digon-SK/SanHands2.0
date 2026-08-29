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


def cross(a, b):
    return Vector(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    )


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


def unwrap_uv_axis(values):
    """Keep a UV island together when it crosses the repeating 0/1 seam."""
    if len(values) < 2 or max(values) - min(values) <= 0.75:
        return list(values)
    wrapped = sorted(value % 1.0 for value in values)
    gaps = [
        ((wrapped[(index + 1) % len(wrapped)] - wrapped[index]) % 1.0, index)
        for index in range(len(wrapped))
    ]
    _, gap_index = max(gaps)
    start = wrapped[(gap_index + 1) % len(wrapped)]
    unwrapped = [value % 1.0 + (1.0 if value % 1.0 < start else 0.0) for value in values]
    if max(unwrapped) - min(unwrapped) >= max(values) - min(values):
        return list(values)
    raw_center = (min(values) + max(values)) * 0.5
    new_center = (min(unwrapped) + max(unwrapped)) * 0.5
    tile = round(raw_center - new_center)
    return [value + tile for value in unwrapped]


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    amount = position - lower
    return ordered[lower] * (1.0 - amount) + ordered[upper] * amount


def remap_uv_island(source_layer, target_samples):
    """Affine-map the complete source hand island into the old hand's skin island."""
    source_u = unwrap_uv_axis([uv.u for uv in source_layer])
    source_v = unwrap_uv_axis([uv.v for uv in source_layer])
    target_u = unwrap_uv_axis([uv.u for uv in target_samples])
    target_v = unwrap_uv_axis([uv.v for uv in target_samples])

    def map_axis(values, source_values, target_values):
        source_min, source_max = min(source_values), max(source_values)
        target_min = percentile(target_values, 0.10)
        target_max = percentile(target_values, 0.90)
        padding = (target_max - target_min) * 0.075
        target_min -= padding
        target_max += padding
        if source_max - source_min < 1e-8:
            return [(target_min + target_max) * 0.5 for _ in values]
        return [
            target_min
            + (value - source_min) / (source_max - source_min) * (target_max - target_min)
            for value in values
        ]

    mapped_u = map_axis(source_u, source_u, target_u)
    mapped_v = map_axis(source_v, source_v, target_v)
    return [TexCoords(u, v) for u, v in zip(mapped_u, mapped_v)]


def uv_triangle_center(layer, triangle):
    indices = (triangle.a, triangle.b, triangle.c)
    values_u = unwrap_uv_axis([layer[index].u for index in indices])
    values_v = unwrap_uv_axis([layer[index].v for index in indices])
    return TexCoords(sum(values_u) / 3.0, sum(values_v) / 3.0)


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
    # Select only the original hand surface. Including forearm weights here can
    # remove a complete low-poly wrist triangle and leave a visible gap. A
    # triangle must have meaningful hand/finger influence and its centroid must
    # sit at or beyond the replacement mesh's wrist overlap plane.
    hand_indices = {
        id_to_bone[config["hand"]].index,
        id_to_bone[config["finger"]].index,
        id_to_bone[config["finger1"]].index,
    }
    arm_indices = hand_indices | {target_forearm_index}
    hand_weights = [
        sum(weight for bone_index, weight in zip(bone_indices, weights) if bone_index in hand_indices)
        for bone_indices, weights in zip(skin.vertex_bone_indices, skin.vertex_bone_weights)
    ]
    local_x = [mat_vec(inverse_forearm, vertex).x for vertex in geo.vertices]
    removed = []
    for tri in geo.triangles:
        indices = (tri.a, tri.b, tri.c)
        centroid_x = sum(local_x[index] for index in indices) / 3.0
        if max(hand_weights[index] for index in indices) > 0.20 and centroid_x >= cut - 0.002:
            removed.append(tri)
    # A handful of original models have one entire hand incorrectly weighted
    # to the forearm. For those sides only, use the same centroid plane with
    # the complete arm influence. The centroid test preserves every triangle
    # crossing back into the forearm instead of opening the old wrist gap.
    if len(removed) < 8:
        arm_weights = [
            sum(
                weight
                for bone_index, weight in zip(bone_indices, weights)
                if bone_index in arm_indices
            )
            for bone_indices, weights in zip(
                skin.vertex_bone_indices, skin.vertex_bone_weights
            )
        ]
        removed = []
        for tri in geo.triangles:
            indices = (tri.a, tri.b, tri.c)
            centroid_x = sum(local_x[index] for index in indices) / 3.0
            if (
                max(arm_weights[index] for index in indices) > 0.20
                and centroid_x >= cut - 0.002
            ):
                removed.append(tri)
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
        "arm_indices": arm_indices,
        "removed": removed,
        "replacement_bones": replacement_bones,
        "replace_start_id": config["finger"],
        "replace_skip_id": config["finger1"],
        "new_bones": new_bones,
        "new_frames": new_frames,
        "matrix_updates": matrix_updates,
        "skin_index_map": skin_index_map,
    }


def unique_positions(items, vertices, tolerance_sq=1e-8):
    result = []
    for index in items:
        if not any(distance_sq(vertices[index], vertices[other]) <= tolerance_sq for other in result):
            result.append(index)
    return result


def target_wrist_ring(plan, old_vertices, old_indices, old_weights, used_old):
    source_vertices = plan["template"]["geo"].vertices
    source_min_x = min(vertex.x for vertex in source_vertices)
    inverse_transform = inverse(plan["transform"])
    removed_indices = {
        index for tri in plan["removed"] for index in (tri.a, tri.b, tri.c)
    }
    boundary = []
    # Most models expose their wrist contour very close to the template plane.
    # A few beta/custom peds have a much longer hand offset, so widen the search
    # only when the tight contour does not contain a usable ring.
    for window in (0.08, 0.18):
        candidates = []
        for index in used_old:
            arm_weight = sum(
                weight
                for bone_index, weight in zip(old_indices[index], old_weights[index])
                if bone_index in plan["arm_indices"]
            )
            local = mat_vec(inverse_transform, old_vertices[index])
            if arm_weight > 0.20 and abs(local.x - source_min_x) <= window:
                candidates.append(index)
        boundary = [
            index
            for index in candidates
            if min(
                distance_sq(old_vertices[index], old_vertices[removed_index])
                for removed_index in removed_indices
            )
            <= 1e-8
        ]
        boundary = unique_positions(boundary, old_vertices)
        if len(boundary) >= 3:
            break
    if len(boundary) < 3:
        raise ValueError(f"Only {len(boundary)} unique {plan['side']} wrist boundary vertices")
    return boundary


def order_ring(indices, vertices, transform=None):
    points = [mat_vec(transform, vertices[index]) if transform else vertices[index] for index in indices]
    center_y = sum(point.y for point in points) / len(points)
    center_z = sum(point.z for point in points) / len(points)
    ordered = sorted(
        zip(indices, points),
        key=lambda item: math.atan2(item[1].z - center_z, item[1].y - center_y),
    )
    return [index for index, _ in ordered], center_y, center_z


def append_wrist_bridge(geo, plan, source_output_indices, target_output_indices, material):
    inverse_transform = inverse(plan["transform"])
    source_order, source_center_y, source_center_z = order_ring(
        source_output_indices,
        geo.vertices,
        inverse_transform,
    )
    target_order, target_center_y, target_center_z = order_ring(
        target_output_indices,
        geo.vertices,
        inverse_transform,
    )
    center_y = (source_center_y + target_center_y) * 0.5
    center_z = (source_center_z + target_center_z) * 0.5

    def angle(index):
        point = mat_vec(inverse_transform, geo.vertices[index])
        return math.atan2(point.z - center_z, point.y - center_y) % (2.0 * math.pi)

    # Start both loops at the source point nearest angle zero, then walk them
    # together. Advancing the loop whose next polar angle comes first creates a
    # closed zipper even when the original wrist has fewer vertices.
    offset = min(angle(index) for index in source_order)

    def with_relative_angles(indices):
        rows = [(((angle(index) - offset) % (2.0 * math.pi)), index) for index in indices]
        rows.sort()
        return rows

    source_rows = with_relative_angles(source_order)
    target_rows = with_relative_angles(target_order)
    source_order = [index for _, index in source_rows]
    target_order = [index for _, index in target_rows]
    source_angles = [value for value, _ in source_rows]
    target_angles = [value for value, _ in target_rows]
    source_count, target_count = len(source_order), len(target_order)
    i = j = 0

    def add_outward_triangle(a, b, c):
        pa = mat_vec(inverse_transform, geo.vertices[a])
        pb = mat_vec(inverse_transform, geo.vertices[b])
        pc = mat_vec(inverse_transform, geo.vertices[c])
        normal = cross(sub(pb, pa), sub(pc, pa))
        radial = Vector(
            0.0,
            (pa.y + pb.y + pc.y) / 3.0 - center_y,
            (pa.z + pb.z + pc.z) / 3.0 - center_z,
        )
        if dot(normal, radial) < 0.0:
            b, c = c, b
        # DragonFF's Triangle tuple is stored as (b, a, material, c).
        geo.triangles.append(Triangle(b, a, material, c))

    while i < source_count or j < target_count:
        current_source = source_order[i % source_count]
        current_target = target_order[j % target_count]
        next_source_angle = (
            source_angles[i + 1] if i + 1 < source_count else source_angles[0] + 2.0 * math.pi
        )
        next_target_angle = (
            target_angles[j + 1] if j + 1 < target_count else target_angles[0] + 2.0 * math.pi
        )
        if i < source_count and (j >= target_count or next_source_angle <= next_target_angle):
            next_source = source_order[(i + 1) % source_count]
            add_outward_triangle(current_source, next_source, current_target)
            i += 1
        else:
            next_target = target_order[(j + 1) % target_count]
            add_outward_triangle(current_source, next_target, current_target)
            j += 1


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
        material = Counter(tri.material for tri in plan["removed"]).most_common(1)[0][0]
        material_triangles = [tri for tri in plan["removed"] if tri.material == material]
        nearest = []
        for source_vertex in source_geo.vertices:
            transformed = mat_vec(plan["transform"], source_vertex)
            geo.vertices.append(transformed)
            nearest.append(nearest_surface_data(transformed, plan["removed"], old_vertices))

        if old_normals:
            for source_normal in source_geo.normals:
                geo.normals.append(normalize(mat_vec(plan["transform"], source_normal, 0.0)))

        for layer_index, old_layer in enumerate(old_uv_layers):
            if source_geo.uv_layers:
                source_layer = source_geo.uv_layers[min(layer_index, len(source_geo.uv_layers) - 1)]
                target_samples = [
                    uv_triangle_center(old_layer, tri)
                    for tri in material_triangles
                ]
                geo.uv_layers[layer_index].extend(
                    remap_uv_island(source_layer, target_samples)
                )
            else:
                for _, indices, weights in nearest:
                    geo.uv_layers[layer_index].append(
                        interpolate(old_layer, indices, weights, TexCoords)
                    )

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

        for source_tri in source_geo.triangles:
            geo.triangles.append(
                Triangle(
                    source_tri.b + base,
                    source_tri.a + base,
                    material,
                    source_tri.c + base,
                )
            )

        source_min_x = min(vertex.x for vertex in source_geo.vertices)
        source_wrist = [
            base + index
            for index, vertex in enumerate(source_geo.vertices)
            if vertex.x <= source_min_x + 0.011
        ]
        target_wrist = [
            remap[index]
            for index in target_wrist_ring(
                plan,
                old_vertices,
                old_indices,
                old_weights,
                used_old,
            )
        ]
        append_wrist_bridge(geo, plan, source_wrist, target_wrist, material)

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
