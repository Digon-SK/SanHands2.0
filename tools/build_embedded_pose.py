#!/usr/bin/env python3
"""Remap SanHands' native hand pose tracks to the embedded ped bone IDs."""

from __future__ import annotations

import argparse
import copy
import sys
from pathlib import Path


SOURCE_FINGER_IDS = tuple(range(3, 18))


def target_bone_id(side: str, source_id: int) -> int:
    if source_id not in SOURCE_FINGER_IDS:
        raise ValueError(f"ID de dedo de origen inesperado: {source_id}")
    if side == "L":
        return {3: 35, 4: 36}.get(source_id, 1000 + source_id)
    return {3: 25, 4: 26}.get(source_id, 1100 + source_id)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rwfury-root", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(args.rwfury_root.resolve()))
    from rwfury import Ifp

    source = Ifp.from_bytes(args.source.read_bytes())
    result = Ifp()
    result.internal_name = "HANDIES"
    result.animations = []

    for source_name, target_name, side in (
        ("LHGrip", "LHGripPed", "L"),
        ("RHGrip", "RHGripPed", "R"),
    ):
        animation = copy.deepcopy(source.get_animation(source_name))
        animation.name = target_name
        animation.objects = [
            obj for obj in animation.objects if obj.bone_id in SOURCE_FINGER_IDS
        ]
        for obj in animation.objects:
            obj.bone_id = target_bone_id(side, obj.bone_id)
        expected = {target_bone_id(side, bone_id) for bone_id in SOURCE_FINGER_IDS}
        actual = {obj.bone_id for obj in animation.objects}
        if actual != expected:
            raise SystemExit(
                f"{target_name}: IDs incorrectos: faltan={sorted(expected - actual)} "
                f"sobran={sorted(actual - expected)}"
            )
        result.animations.append(animation)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    raw = result.to_bytes()
    check = Ifp.from_bytes(raw)
    if check.internal_name.upper() != "HANDIES":
        raise SystemExit("El bloque IFP generado no se llama HANDIES")
    if [animation.name for animation in check.animations] != [
        "LHGripPed",
        "RHGripPed",
    ]:
        raise SystemExit("Las animaciones integradas no se validaron")
    args.output.write_bytes(raw)
    print(f"Generado {args.output} ({len(raw)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
