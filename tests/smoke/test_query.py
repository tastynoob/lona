from __future__ import annotations

import json
import subprocess
from pathlib import Path


def send_command(proc: subprocess.Popen[str], command: str) -> dict:
    assert proc.stdin is not None
    assert proc.stdout is not None
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    assert line, f"no reply for command {command!r}"
    return json.loads(line)


def test_query_can_switch_active_module_and_reload_by_canonical_path(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    root_path = app_dir / "main.lo"
    helper_path = lib_dir / "helper.lo"
    root_path.write_text(
        "\n".join(
            [
                "import helper",
                "",
                "def main() i32 {",
                "    ret helper.value()",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "def value() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir), str(lib_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        status = send_command(proc, "status")
        assert status["ok"] is True, status
        assert status["result"]["rootPaths"] == [str(app_dir), str(lib_dir)], status
        assert status["result"]["entryModules"] == [], status
        assert status["result"]["path"] is None, status

        gotom = send_command(proc, "gotom helper")
        assert gotom["ok"] is True, gotom
        assert gotom["result"]["rootPaths"] == [str(app_dir), str(lib_dir)], gotom
        assert gotom["result"]["path"] == str(helper_path), gotom
        assert gotom["result"]["cursorLine"] is None, gotom

        gotom_root = send_command(proc, "gotom main")
        assert gotom_root["ok"] is True, gotom_root
        assert gotom_root["result"]["path"] == str(root_path), gotom_root

        ast_reply = send_command(proc, "ast")
        assert ast_reply["ok"] is True, ast_reply
        assert ast_reply["result"]["path"] == str(root_path), ast_reply

        print_reply = send_command(proc, "print main")
        assert print_reply["ok"] is True, print_reply
        assert print_reply["result"]["item"]["kind"] == "func", print_reply
        assert print_reply["result"]["item"]["name"] == "main", print_reply

        reload_reply = send_command(proc, "reload helper")
        assert reload_reply["ok"] is True, reload_reply

        bad_suffix = send_command(proc, "reload helper.lo")
        assert bad_suffix["ok"] is False, bad_suffix
        assert "omit the file suffix" in bad_suffix["result"]["error"], bad_suffix

        bad_abs = send_command(proc, f"gotom {helper_path}")
        assert bad_abs["ok"] is False, bad_abs
        assert "canonical module paths" in bad_abs["result"]["error"], bad_abs

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
    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"


def test_query_reload_from_dependency_keeps_root_semantic_diagnostics(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    root_path = app_dir / "main.lo"
    helper_path = lib_dir / "helper.lo"
    root_path.write_text(
        "\n".join(
            [
                "import helper",
                "",
                "def main() i32 {",
                "    ret helper.value()",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "def value() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir), str(lib_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        open_root = send_command(proc, "gotom main")
        assert open_root["ok"] is True, open_root

        gotom = send_command(proc, "gotom helper")
        assert gotom["ok"] is True, gotom

        helper_path.write_text(
            "\n".join(
                [
                    "def value(seed i32) i32 {",
                    "    ret seed",
                    "}",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        reload_reply = send_command(proc, "reload helper")
        assert reload_reply["ok"] is True, reload_reply
        assert reload_reply["result"]["path"] == str(helper_path), reload_reply
        assert reload_reply["result"]["hasAnalysis"] is True, reload_reply

        status = send_command(proc, "status")
        assert status["ok"] is True, status
        assert status["result"]["path"] == str(helper_path), status
        assert status["result"]["diagnosticCount"] >= 1, status

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics
        assert diagnostics["result"]["count"] >= 1, diagnostics
        assert (
            diagnostics["result"]["items"][0]["location"]["path"] == str(root_path)
        ), diagnostics

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
    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"


def test_query_analyzes_clean_dependency_even_with_root_semantic_error(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    root_path = app_dir / "main.lo"
    helper_path = lib_dir / "helper.lo"
    root_path.write_text(
        "\n".join(
            [
                "import helper",
                "",
                "def main() i32 {",
                "    ret helper.value(1)",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "def value() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir), str(lib_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        open_root = send_command(proc, "gotom main")
        assert open_root["ok"] is True, open_root

        status = send_command(proc, "status")
        assert status["ok"] is True, status
        assert status["result"]["diagnosticCount"] >= 1, status

        gotom = send_command(proc, "gotom helper")
        assert gotom["ok"] is True, gotom
        assert gotom["result"]["path"] == str(helper_path), gotom
        assert gotom["result"]["hasAnalysis"] is True, gotom

        print_reply = send_command(proc, "print value")
        assert print_reply["ok"] is True, print_reply
        assert print_reply["result"]["item"]["kind"] == "func", print_reply
        assert print_reply["result"]["item"]["name"] == "value", print_reply

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
    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"


def test_query_gotom_tracks_new_broken_entry_for_diagnostics(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    helper_path = lib_dir / "helper.lo"
    helper_path.write_text(
        "\n".join(
            [
                "import missing_dep",
                "",
                "def value() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir), str(lib_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        gotom = send_command(proc, "gotom helper")
        assert gotom["ok"] is True, gotom
        assert gotom["result"]["path"] == str(helper_path), gotom
        assert str(helper_path) in gotom["result"]["entryModules"], gotom
        assert gotom["result"]["diagnosticCount"] >= 1, gotom

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics
        assert diagnostics["result"]["count"] >= 1, diagnostics
        assert (
            diagnostics["result"]["items"][0]["location"]["path"] == str(helper_path)
        ), diagnostics

        ast_reply = send_command(proc, "ast")
        assert ast_reply["ok"] is True, ast_reply
        assert ast_reply["result"]["path"] == str(helper_path), ast_reply

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
    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"


def test_query_reload_refreshes_later_entries_after_earlier_failure(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    root_path = app_dir / "main.lo"
    helper_path = lib_dir / "helper.lo"
    root_path.write_text(
        "\n".join(
            [
                "def main() i32 {",
                "    ret 1",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "def value() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir), str(lib_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        helper_open = send_command(proc, "gotom helper")
        assert helper_open["ok"] is True, helper_open

        root_open = send_command(proc, "gotom main")
        assert root_open["ok"] is True, root_open
        assert root_open["result"]["path"] == str(root_path), root_open

        helper_path.write_text(
            "\n".join(
                [
                    "import missing_dep",
                    "",
                    "def value() i32 {",
                    "    ret 7",
                    "}",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        root_path.write_text(
            "\n".join(
                [
                    "def renamed() i32 {",
                    "    ret 2",
                    "}",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        reload_reply = send_command(proc, "reload")
        assert reload_reply["ok"] is True, reload_reply
        assert reload_reply["result"]["path"] == str(root_path), reload_reply
        assert reload_reply["result"]["diagnosticCount"] >= 1, reload_reply

        print_renamed = send_command(proc, "print renamed")
        assert print_renamed["ok"] is True, print_renamed
        assert print_renamed["result"]["item"]["name"] == "renamed", print_renamed

        print_old = send_command(proc, "print main")
        assert print_old["ok"] is False, print_old

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics
        assert diagnostics["result"]["count"] >= 1, diagnostics
        assert (
            diagnostics["result"]["items"][0]["location"]["path"] == str(helper_path)
        ), diagnostics

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
    assert proc.returncode == 0, stderr or f"unexpected return code {proc.returncode}"
