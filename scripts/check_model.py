#!/usr/bin/env python3
"""Check declared model resources before the SDK loads them. No model files modified."""
import json
import sys
from pathlib import Path


def validate(path):
    path = Path(path).resolve(strict=True)
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict) or data.get("Version") != 3:
        raise ValueError("Expected Version: 3 in model3.json")
    refs = data.get("FileReferences")
    if not isinstance(refs, dict):
        raise ValueError("Missing FileReferences")
    files = []
    def add(value, name, optional=False):
        if optional and (value is None or value == ""):
            return
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"Invalid {name} reference")
        p = Path(value)
        if p.is_absolute() or "\\" in value:
            raise ValueError(f"{name}: expected a Linux-compatible relative path: {value}")
        if not (path.parent / p).is_file():
            raise ValueError(f"Missing {name}: {path.parent / p}")
        files.append(value)
    add(refs.get("Moc"), "Moc")
    textures = refs.get("Textures")
    if not isinstance(textures, list) or not textures:
        raise ValueError("Expected nonempty Textures array")
    for texture in textures:
        add(texture, "Texture")
    for field in ("Physics", "Pose", "UserData", "DisplayInfo"):
        add(refs.get(field), field, optional=True)
    for expression in refs.get("Expressions", []):
        add(expression["File"], "Expression")
    for motions in refs.get("Motions", {}).values():
        for motion in motions:
            add(motion["File"], "Motion")
            add(motion.get("Sound"), "Sound", optional=True)
    return files

if __name__ == "__main__":
    try:
        files = validate(sys.argv[1])
        print(f"[studio] model manifest OK: {len(files)} resource references", flush=True)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, IndexError) as error:
        print(f"[studio] {error}", file=sys.stderr)
        sys.exit(1)
