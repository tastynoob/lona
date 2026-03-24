from __future__ import annotations

import os
from pathlib import Path

import pytest

from tests.harness.compiler import CompilerHarness


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def compiler_bin(repo_root: Path) -> Path:
    path = repo_root / "build" / "lona-ir"
    if not path.is_file():
        pytest.fail(f"missing compiler binary: {path}")
    if not os.access(path, os.X_OK):
        pytest.fail(f"compiler binary is not executable: {path}")
    return path


@pytest.fixture(scope="session")
def fixtures_dir(repo_root: Path) -> Path:
    return repo_root / "tests" / "fixtures"


@pytest.fixture(scope="session")
def system_driver(repo_root: Path) -> Path:
    path = repo_root / "scripts" / "lac.sh"
    if not path.is_file():
        pytest.fail(f"missing system driver: {path}")
    if not os.access(path, os.X_OK):
        pytest.fail(f"system driver is not executable: {path}")
    return path


@pytest.fixture
def compiler(repo_root: Path, compiler_bin: Path, system_driver: Path, tmp_path: Path) -> CompilerHarness:
    return CompilerHarness(
        repo_root=repo_root,
        compiler_bin=compiler_bin,
        system_driver=system_driver,
        tmp_path=tmp_path,
    )
