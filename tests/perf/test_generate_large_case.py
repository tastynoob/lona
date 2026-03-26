from __future__ import annotations

from tests.perf.generate_large_case import write_case


def test_generated_large_case_matches_manifest_and_compiles(compiler, tmp_path) -> None:
    input_path = tmp_path / "fixed-large-100k.lo"
    manifest = compiler.repo_root / "tests" / "perf" / "large_case_manifest.json"

    metadata = write_case(input_path, manifest)
    assert metadata["generator_version"] == 4

    compiler.emit_ir(input_path).expect_ok()
