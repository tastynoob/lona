from __future__ import annotations

import re
from pathlib import Path

from tests.harness.compiler import run_command


def test_build_lona_skill_generates_skill_tree(repo_root: Path, tmp_path: Path) -> None:
    output_dir = tmp_path / "lona-author"
    result = run_command(
        [
            "python3",
            str(repo_root / "scripts" / "build_lona_skill.py"),
            "--output",
            str(output_dir),
        ],
        cwd=repo_root,
    )
    result.expect_ok()

    skill_md = output_dir / "SKILL.md"
    skill_index = output_dir / "references" / "skill-index.md"

    assert skill_md.is_file()
    assert skill_index.is_file()
    assert (output_dir / "references" / "language" / "grammar.md").is_file()
    assert (output_dir / "references" / "language" / "type.md").is_file()
    assert (output_dir / "references" / "runtime" / "commands.md").is_file()
    assert (output_dir / "references" / "runtime" / "native_build.md").is_file()
    assert (output_dir / "references" / "runtime" / "c_ffi.md").is_file()
    assert not (output_dir / "references" / "internals").exists()
    assert not (output_dir / "references" / "proposals").exists()
    assert not (output_dir / "references" / "archive").exists()

    skill_text = skill_md.read_text(encoding="utf-8")
    index_text = skill_index.read_text(encoding="utf-8")

    assert "name: lona-author" in skill_text
    assert "references/language/" in skill_text
    assert "references/runtime/commands.md" in skill_text
    assert "references/runtime/native_build.md" in skill_text
    assert "runtime command" in skill_text
    assert "Do not infer unsupported language features from compiler internals" in skill_text
    assert "example/" not in skill_text
    assert "tests/" not in skill_text
    assert "repository" not in skill_text
    assert "## Source Priority" in index_text
    assert "[language/grammar.md](language/grammar.md)" in index_text
    assert "[runtime/commands.md](runtime/commands.md)" in index_text
    assert "[runtime/native_build.md](runtime/native_build.md)" in index_text

    all_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted(output_dir.rglob("*.md"))
    )
    forbidden = [
        r"/home/",
        r"\brepository\b",
        r"\brepo\b",
        r"build/lona-ir",
        r"scripts/lac\.sh",
        r"scripts/lac-native\.sh",
        r"runtime/bare_x86_64/",
        r"src/lona/",
        r"\.\./\.\./internals/",
        r"example/",
        r"tests/",
    ]
    for pattern in forbidden:
        assert re.search(pattern, all_text) is None, pattern
