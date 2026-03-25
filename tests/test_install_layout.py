from __future__ import annotations

from pathlib import Path

from tests.harness.compiler import run_command


def test_make_install_does_not_install_bare_runtime_assets(repo_root: Path, tmp_path: Path) -> None:
    destdir = tmp_path / "install-root"
    result = run_command(
        ["make", "install", f"DESTDIR={destdir}", "PREFIX=/usr/local"],
        cwd=repo_root,
    )
    result.expect_ok()

    bindir = destdir / "usr/local/bin"
    assert (bindir / "lona-ir").is_file()
    assert (bindir / "lac").is_file()
    assert (bindir / "lac-native").is_file()

    runtime_root = destdir / "usr/local/share/lona/runtime/bare_x86_64"
    assert not runtime_root.exists()
