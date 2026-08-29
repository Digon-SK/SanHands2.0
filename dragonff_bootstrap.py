"""Locate DragonFF's standalone gtaLib package without bundling DragonFF."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def configure_dragonff() -> Path:
    candidates = []
    if "--dragonff" in sys.argv:
        index = sys.argv.index("--dragonff")
        if index + 1 < len(sys.argv):
            candidates.append(Path(sys.argv[index + 1]))
    if os.environ.get("DRAGONFF_PATH"):
        candidates.append(Path(os.environ["DRAGONFF_PATH"]))

    project = Path(__file__).resolve().parent
    candidates.extend((project / "DragonFF", project / "DragonFF-master"))

    for candidate in candidates:
        candidate = candidate.expanduser().resolve()
        if (candidate / "gtaLib" / "dff.py").is_file():
            sys.path.insert(0, str(candidate))
            return candidate

    raise SystemExit(
        "DragonFF was not found. Pass --dragonff PATH or set DRAGONFF_PATH "
        "to the extracted DragonFF directory."
    )
