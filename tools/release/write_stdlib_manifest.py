from __future__ import annotations

import hashlib
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: write_stdlib_manifest.py ROOT VERSION OUTPUT")
    root = pathlib.Path(sys.argv[1]).resolve()
    version = sys.argv[2]
    output = pathlib.Path(sys.argv[3])
    files = []
    aggregate = hashlib.sha256()
    for path in sorted(root.rglob("*.bas"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        content_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        files.append({"path": relative, "sha256": content_hash})
        aggregate.update(relative.encode("utf-8"))
        aggregate.update(b"\0")
        aggregate.update(content_hash.encode("ascii"))
        aggregate.update(b"\n")
    manifest = {
        "format_version": 1,
        "ombasic_version": version,
        "content_sha256": aggregate.hexdigest(),
        "files": files,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
