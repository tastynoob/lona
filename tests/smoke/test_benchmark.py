from __future__ import annotations

import time

from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_benchmark_smoke_cases_report_stats(compiler: CompilerHarness, repo_root) -> None:
    synthetic = compiler.write_source(
        "benchmark/synthetic.lo",
        "\n".join(
            [
                "def seed() i32 {",
                "    ret 0",
                "}",
                "",
                *[
                    (
                        "def chain0(v i32) i32 {\n    ret v + 1\n}\n"
                        if i == 0
                        else f"def chain{i}(v i32) i32 {{\n    ret chain{i - 1}(v) + 1\n}}\n"
                    )
                    for i in range(200)
                ],
                "def main() i32 {",
                "    ret chain199(seed())",
                "}",
                "",
            ]
        ),
    )

    for name, input_path in [
        ("function-pointer", repo_root / "example" / "function_pointer_suite.lo"),
        ("module-import", repo_root / "example" / "modules" / "main.lo"),
        ("synthetic-chain", synthetic),
    ]:
        start = time.perf_counter_ns()
        result = compiler.emit_ir(input_path, stats=True).expect_ok()
        elapsed_ms = (time.perf_counter_ns() - start) // 1_000_000
        print(f"[{name}]")
        print(f"wall-ms: {elapsed_ms}")
        print(result.stderr.strip() or "<no stats>")
        print()
        assert_contains(result.stderr, "compiled-modules", label=f"{name} stats")
        assert_contains(result.stderr, "reused-modules", label=f"{name} stats")
        assert_contains(result.stderr, "dependency-scan-ms", label=f"{name} stats")
        assert_contains(result.stderr, "resolve-ms", label=f"{name} stats")
        assert_contains(result.stderr, "analyze-ms", label=f"{name} stats")
        assert_contains(result.stderr, "artifact-emit-ms", label=f"{name} stats")
        assert_contains(result.stderr, "output-render-ms", label=f"{name} stats")
        assert_contains(result.stderr, "output-write-ms", label=f"{name} stats")
        assert_contains(result.stderr, "cache-lookup-ms", label=f"{name} stats")
        assert_contains(result.stderr, "link-load-ms", label=f"{name} stats")
        assert_contains(result.stderr, "link-merge-ms", label=f"{name} stats")
