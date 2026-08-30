#!/usr/bin/env python3
"""Reimport and structurally validate the editable Handies blendshape GLB."""

from __future__ import annotations

import math
import sys
from pathlib import Path

import bpy


def main() -> int:
    if "--" not in sys.argv or len(sys.argv) <= sys.argv.index("--") + 1:
        raise SystemExit("Pass the GLB path after --")
    path = Path(sys.argv[sys.argv.index("--") + 1])
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.import_scene.gltf(filepath=str(path))

    expected = {
        "Slim_Left": (158, ["Basis", "Grip", *[f"LHGsign{i}" for i in range(1, 6)]]),
        "Slim_Right": (
            148,
            ["Basis", "Grip", "FuckU", *[f"RHGsign{i}" for i in range(1, 6)]],
        ),
        "Fat_Left": (150, ["Basis", "Grip", *[f"LHGsign{i}" for i in range(1, 6)]]),
        "Fat_Right": (
            140,
            ["Basis", "Grip", "FuckU", *[f"RHGsign{i}" for i in range(1, 6)]],
        ),
    }
    errors = []
    for object_name, (vertex_count, key_names) in expected.items():
        obj = bpy.data.objects.get(object_name)
        if obj is None or obj.type != "MESH":
            errors.append(f"missing mesh {object_name}")
            continue
        if len(obj.data.vertices) != vertex_count:
            errors.append(
                f"{object_name}: vertices={len(obj.data.vertices)} expected={vertex_count}"
            )
        if len(obj.data.uv_layers) != 1:
            errors.append(f"{object_name}: expected one UV layer")
        shape_keys = obj.data.shape_keys
        actual_keys = (
            [] if shape_keys is None else [key.name for key in shape_keys.key_blocks]
        )
        if actual_keys != key_names:
            errors.append(f"{object_name}: keys={actual_keys} expected={key_names}")
            continue
        basis = shape_keys.key_blocks[0]
        for key in shape_keys.key_blocks[1:]:
            maximum_delta = 0.0
            for base_vertex, target_vertex in zip(basis.data, key.data):
                delta = target_vertex.co - base_vertex.co
                if not all(math.isfinite(component) for component in delta):
                    errors.append(f"{object_name}.{key.name}: non-finite coordinate")
                    break
                maximum_delta = max(maximum_delta, delta.length)
            if maximum_delta <= 1.0e-6:
                errors.append(f"{object_name}.{key.name}: empty profile")

    unexpected = sorted(
        obj.name for obj in bpy.context.scene.objects if obj.name not in expected
    )
    if unexpected:
        errors.append(f"unexpected objects: {unexpected}")
    print(
        f"BLENDSHAPE_VALIDATION file={path} objects={len(expected)} "
        f"errors={len(errors)}"
    )
    for error in errors:
        print(f"ERROR {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
