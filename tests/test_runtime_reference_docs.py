from __future__ import annotations

import re
from pathlib import Path


def test_runtime_reference_docs_avoid_repo_private_paths(repo_root: Path) -> None:
    docs = [
        repo_root / "docs" / "reference" / "runtime" / "commands.md",
        repo_root / "docs" / "reference" / "runtime" / "native_build.md",
        repo_root / "docs" / "reference" / "runtime" / "c_ffi.md",
    ]

    all_text = "\n".join(path.read_text(encoding="utf-8") for path in docs)
    forbidden = [
        r"/home/",
        r"\brepository\b",
        r"\brepo\b",
        r"build/",
        r"scripts/",
        r"runtime/bare_x86_64/",
        r"src/lona/",
        r"\.\./\.\./internals/",
        r"当前仓库",
        r"仓库根目录",
        r"example/",
        r"tests/",
    ]
    for pattern in forbidden:
        assert re.search(pattern, all_text) is None, pattern
