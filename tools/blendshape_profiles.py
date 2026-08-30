"""Binary interchange format for editable Handies blendshape templates."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"HBSHAPE\0"
VERSION = 1


@dataclass(frozen=True)
class MorphTarget:
    name: str
    positions: tuple[tuple[float, float, float], ...]
    normals: tuple[tuple[float, float, float], ...]


@dataclass(frozen=True)
class MorphTemplate:
    name: str
    targets: tuple[MorphTarget, ...]

    @property
    def vertex_count(self) -> int:
        return len(self.targets[0].positions)

    def target(self, name: str) -> MorphTarget:
        folded = name.casefold()
        for target in self.targets:
            if target.name.casefold() == folded:
                return target
        raise KeyError(f"{self.name}: missing morph target {name}")


def write_profiles(path: Path, templates: tuple[MorphTemplate, ...]) -> None:
    data = bytearray(MAGIC)
    data += struct.pack("<II", VERSION, len(templates))
    for template in templates:
        encoded_template = template.name.encode("ascii")
        data += struct.pack(
            "<III", len(encoded_template), template.vertex_count, len(template.targets)
        )
        data += encoded_template
        for target in template.targets:
            encoded_target = target.name.encode("ascii")
            if len(target.positions) != template.vertex_count:
                raise ValueError(f"{template.name}.{target.name}: position count mismatch")
            if len(target.normals) != template.vertex_count:
                raise ValueError(f"{template.name}.{target.name}: normal count mismatch")
            data += struct.pack("<I", len(encoded_target))
            data += encoded_target
            for value in target.positions:
                data += struct.pack("<3f", *value)
            for value in target.normals:
                data += struct.pack("<3f", *value)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def load_profiles(path: Path) -> tuple[MorphTemplate, ...]:
    data = memoryview(path.read_bytes())
    offset = 0

    def read(fmt: str):
        nonlocal offset
        size = struct.calcsize(fmt)
        if offset + size > len(data):
            raise ValueError(f"{path}: truncated blendshape data")
        result = struct.unpack_from(fmt, data, offset)
        offset += size
        return result

    def read_text(length: int) -> str:
        nonlocal offset
        if length <= 0 or length > 63 or offset + length > len(data):
            raise ValueError(f"{path}: invalid blendshape name length {length}")
        result = bytes(data[offset : offset + length]).decode("ascii")
        offset += length
        return result

    if len(data) < len(MAGIC) or bytes(data[: len(MAGIC)]) != MAGIC:
        raise ValueError(f"{path}: invalid blendshape magic")
    offset = len(MAGIC)
    version, template_count = read("<II")
    if version != VERSION or template_count == 0 or template_count > 16:
        raise ValueError(f"{path}: unsupported blendshape header")
    templates = []
    names = set()
    for _ in range(template_count):
        name_length, vertex_count, target_count = read("<III")
        name = read_text(name_length)
        if name.casefold() in names or vertex_count == 0 or vertex_count > 10000:
            raise ValueError(f"{path}: invalid template {name}")
        if target_count == 0 or target_count > 64:
            raise ValueError(f"{path}: invalid target count for {name}")
        names.add(name.casefold())
        targets = []
        target_names = set()
        for _ in range(target_count):
            (target_name_length,) = read("<I")
            target_name = read_text(target_name_length)
            if target_name.casefold() in target_names:
                raise ValueError(f"{path}: duplicate target {name}.{target_name}")
            target_names.add(target_name.casefold())
            positions = tuple(read("<3f") for _ in range(vertex_count))
            normals = tuple(read("<3f") for _ in range(vertex_count))
            targets.append(MorphTarget(target_name, positions, normals))
        templates.append(MorphTemplate(name, tuple(targets)))
    if offset != len(data):
        raise ValueError(f"{path}: trailing blendshape data")
    return tuple(templates)

