import argparse
from collections import Counter
from pathlib import Path

from dragonff_bootstrap import configure_dragonff


DRAGONFF = configure_dragonff()

from gtaLib.dff import dff  # noqa: E402


def bounds(vertices):
    if not vertices:
        return None
    return tuple(
        (min(getattr(v, axis) for v in vertices), max(getattr(v, axis) for v in vertices))
        for axis in ("x", "y", "z")
    )


def inspect(path: Path) -> None:
    model = dff()
    model.load_file(str(path))
    print(f"FILE {path.name} version={model.rw_version:#x} clumps={len(model.clumps)}")
    for ci, clump in enumerate(model.clumps):
        print(
            f" CLUMP {ci}: frames={len(clump.frame_list)} "
            f"geometries={len(clump.geometry_list)} atomics={len(clump.atomic_list)}"
        )
        for i, frame in enumerate(clump.frame_list):
            header = frame.bone_data.header if frame.bone_data else None
            bone_id = header.id if header else None
            bone_count = header.bone_count if header else 0
            print(
                f"  FRAME {i:02d} parent={frame.parent:2d} id={bone_id!s:>5} "
                f"bones={bone_count:2d} pos=({frame.position.x:.5f},"
                f"{frame.position.y:.5f},{frame.position.z:.5f}) name={frame.name!r}"
            )
            if frame.bone_data and frame.bone_data.bones:
                listing = ", ".join(
                    f"{bone.id}:{bone.index}:{bone.type}" for bone in frame.bone_data.bones
                )
                print(f"   HANIM_BONES {listing}")
        for gi, geometry in enumerate(clump.geometry_list):
            skin = geometry.extensions.get("skin")
            print(
                f"  GEO {gi}: verts={len(geometry.vertices)} tris={len(geometry.triangles)} "
                f"materials={len(geometry.materials)} bounds={bounds(geometry.vertices)}"
            )
            if skin:
                weighted = Counter()
                for indices, weights in zip(skin.vertex_bone_indices, skin.vertex_bone_weights):
                    for index, weight in zip(indices, weights):
                        if weight > 0.00001:
                            weighted[index] += 1
                print(
                    f"   SKIN bones={skin.num_bones} matrices={len(skin.bone_matrices)} "
                    f"used={skin.bones_used} weighted_vertices={dict(weighted)}"
                )
            for mi, material in enumerate(geometry.materials):
                texture = material.textures[0].name if material.textures else None
                print(
                    f"   MATERIAL {mi}: color={material.color} texture={texture!r} "
                    f"surface={material.surface_properties}"
                )
            if geometry.uv_layers:
                uvs = geometry.uv_layers[0]
                print(
                    "   UV0 bounds="
                    f"({min(uv.u for uv in uvs):.5f},{max(uv.u for uv in uvs):.5f}) x "
                    f"({min(uv.v for uv in uvs):.5f},{max(uv.v for uv in uvs):.5f})"
                )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dragonff", type=Path, default=DRAGONFF)
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.paths:
        inspect(path)


if __name__ == "__main__":
    main()
