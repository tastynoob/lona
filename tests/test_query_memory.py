from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest


def query_memcheck_bin() -> Path:
    value = os.environ.get("LONA_QUERY_MEMCHECK_BIN")
    if not value:
        pytest.skip("LONA_QUERY_MEMCHECK_BIN not set")
    path = Path(value)
    if not path.is_file():
        pytest.fail(f"missing lona-query memcheck binary: {path}")
    if not os.access(path, os.X_OK):
        pytest.fail(f"lona-query memcheck binary is not executable: {path}")
    return path


def send_command(proc: subprocess.Popen[str], command: str) -> dict:
    assert proc.stdin is not None
    assert proc.stdout is not None
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    assert line, f"no reply for command {command!r}"
    return json.loads(line)


def test_query_reload_does_not_leak_memory(tmp_path: Path) -> None:
    query_bin = query_memcheck_bin()
    source_path = tmp_path / "reload_memcheck.lo"
    source_path.write_text(
        "\n".join(
            [
                "struct Box {",
                "    value i32",
                "}",
                "",
                "def make() Box {",
                "    ret Box(value = 1)",
                "}",
                "",
                "def main() i32 {",
                "    var box = make()",
                "    ret box.value",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(tmp_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open reload_memcheck")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(source_path), opened

        status = send_command(proc, "status")
        assert status["ok"] is True, status
        assert status["result"]["hasAnalysis"] is True, status

        source_path.write_text(
            "\n".join(
                [
                    "struct Box {",
                    "    value i64",
                    "}",
                    "",
                    "def make() Box {",
                    "    ret Box(value = 2)",
                    "}",
                    "",
                    "def main() i32 {",
                    "    var box = make()",
                    "    ret 0",
                    "}",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        reload_reply = send_command(proc, "reload")
        assert reload_reply["ok"] is True, reload_reply
        assert reload_reply["result"]["hasAnalysis"] is True, reload_reply
        printed_box = send_command(proc, "pt Box")
        assert printed_box["ok"] is True, printed_box
        assert printed_box["result"]["item"]["kind"] == "type", printed_box
        members = printed_box["result"]["item"]["typeInfo"]["members"]
        assert members, printed_box
        assert members[0]["name"] == "value", printed_box
        assert members[0]["type"] == "i64", printed_box

        assert proc.stdin is not None
        proc.stdin.write("quit\n")
        proc.stdin.flush()
        proc.stdin.close()
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)

    stderr = ""
    if proc.stderr is not None:
        stderr = proc.stderr.read()

    if "LeakSanitizer does not work under ptrace" in stderr:
        pytest.skip("LeakSanitizer is unavailable under ptrace in this environment")

    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"
    assert "ERROR: LeakSanitizer" not in stderr, stderr
    assert "ERROR: AddressSanitizer" not in stderr, stderr
