from __future__ import annotations

import re
from pathlib import Path


def _preview(text: str, *, limit: int = 1200) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + "\n...<truncated>..."


def assert_contains(text: str, needle: str, *, label: str) -> None:
    assert needle in text, f"expected {label} to contain {needle!r}\n--- begin {label} ---\n{_preview(text)}\n--- end {label} ---"


def assert_not_contains(text: str, needle: str, *, label: str) -> None:
    assert needle not in text, f"expected {label} not to contain {needle!r}\n--- begin {label} ---\n{_preview(text)}\n--- end {label} ---"


def assert_regex(text: str, pattern: str, *, label: str) -> None:
    assert re.search(pattern, text, re.MULTILINE), (
        f"expected {label} to match /{pattern}/\n--- begin {label} ---\n{_preview(text)}\n--- end {label} ---"
    )


def assert_magic_bytes(path: Path, expected: bytes) -> None:
    actual = path.read_bytes()[: len(expected)]
    assert actual == expected, f"unexpected magic bytes for {path}: expected {expected!r}, got {actual!r}"

