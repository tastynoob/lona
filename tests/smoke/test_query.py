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

        opened = send_command(proc, "open helper")
        assert opened["ok"] is True, opened
        assert opened["result"]["rootPaths"] == [str(app_dir), str(lib_dir)], opened
        assert opened["result"]["path"] == str(helper_path), opened
        assert opened["result"]["cursorLine"] is None, opened

        opened_root = send_command(proc, "open main")
        assert opened_root["ok"] is True, opened_root
        assert opened_root["result"]["path"] == str(root_path), opened_root

        ast_reply = send_command(proc, "ast")
        assert ast_reply["ok"] is True, ast_reply
        assert ast_reply["result"]["path"] == str(root_path), ast_reply

        print_reply = send_command(proc, "pv main")
        assert print_reply["ok"] is True, print_reply
        assert print_reply["result"]["item"]["kind"] == "func", print_reply
        assert print_reply["result"]["item"]["name"] == "main", print_reply

        reload_reply = send_command(proc, "reload helper")
        assert reload_reply["ok"] is True, reload_reply

        bad_suffix = send_command(proc, "reload helper.lo")
        assert bad_suffix["ok"] is False, bad_suffix
        assert "omit the file suffix" in bad_suffix["result"]["error"], bad_suffix

        bad_abs = send_command(proc, f"open {helper_path}")
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
        open_root = send_command(proc, "open main")
        assert open_root["ok"] is True, open_root

        opened = send_command(proc, "open helper")
        assert opened["ok"] is True, opened

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


def test_query_exposes_top_level_inline_constants(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    source_path = app_dir / "main.lo"
    source_path.write_text(
        "\n".join(
            [
                "inline answer i32 = 42",
                "",
                "def main() i32 {",
                "    ret answer",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["symbolCount"] == 2, opened

        symbols = send_command(proc, "info global")
        assert symbols["ok"] is True, symbols
        assert symbols["result"]["count"] == 2, symbols
        inline_items = [
            item
            for item in symbols["result"]["items"]
            if item["kind"] == "global" and item["name"] == "answer"
        ]
        assert len(inline_items) == 1, symbols
        assert inline_items[0]["detail"] == "inline : i32", symbols

        found = send_command(proc, "find all answer")
        assert found["ok"] is True, found
        assert found["result"]["count"] == 1, found
        assert found["result"]["items"][0]["kind"] == "global", found

        printed = send_command(proc, "pv answer")
        assert printed["ok"] is True, printed
        assert printed["result"]["item"]["kind"] == "top-level-var", printed
        assert printed["result"]["item"]["detail"] == "inline", printed
        assert printed["result"]["item"]["type"] == "i32", printed

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


def test_query_can_print_imported_inline_constants(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    lib_dir = tmp_path / "lib"
    app_dir.mkdir()
    lib_dir.mkdir()

    helper_path = lib_dir / "helper.lo"
    root_path = app_dir / "main.lo"
    helper_path.write_text(
        "\n".join(
            [
                "inline answer i32 = 42",
                "",
            ]
        ),
        encoding="utf-8",
    )
    root_path.write_text(
        "\n".join(
            [
                "import helper",
                "",
                "def main() i32 {",
                "    ret helper.answer",
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
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened

        value_reply = send_command(proc, "pv helper.answer")
        assert value_reply["ok"] is True, value_reply
        assert value_reply["result"]["item"]["kind"] == "top-level-var", value_reply
        assert value_reply["result"]["item"]["qualifiedName"] == "helper.answer", value_reply
        assert value_reply["result"]["item"]["detail"] == "inline", value_reply
        assert value_reply["result"]["item"]["type"] == "i32", value_reply
        assert value_reply["result"]["item"]["location"]["path"] == str(helper_path), value_reply

        compat_reply = send_command(proc, "print helper.answer")
        assert compat_reply["ok"] is True, compat_reply
        assert compat_reply["result"]["item"]["qualifiedName"] == "helper.answer", compat_reply

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


def test_query_surfaces_dependency_semantic_errors_at_active_imports(
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
                "    ret 0",
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
                "    ret missing",
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
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened
        assert opened["result"]["diagnosticCount"] >= 2, opened

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics

        items = diagnostics["result"]["items"]
        assert len(items) >= 2, diagnostics

        helper_diagnostic = next(
            item
            for item in items
            if item["location"] is not None
            and item["location"]["path"] == str(helper_path)
        )
        assert helper_diagnostic["category"] == "semantic", helper_diagnostic

        import_bridge = next(
            item
            for item in items
            if item["location"] is not None
            and item["location"]["path"] == str(root_path)
            and item["location"]["line"] == 1
        )
        assert import_bridge["category"] == "semantic", import_bridge
        assert "imported module `helper`" in import_bridge["message"], import_bridge

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


def test_query_surfaces_dependency_syntax_errors_at_active_imports(
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
                "    ret 0",
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
                "    ret 1",
                "    ret",
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
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened
        assert opened["result"]["diagnosticCount"] >= 2, opened

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics

        items = diagnostics["result"]["items"]
        assert len(items) >= 2, diagnostics

        helper_diagnostic = next(
            item
            for item in items
            if item["location"] is not None
            and item["location"]["path"] == str(helper_path)
        )
        assert helper_diagnostic["category"] == "syntax", helper_diagnostic

        import_bridge = next(
            item
            for item in items
            if item["location"] is not None
            and item["location"]["path"] == str(root_path)
            and item["location"]["line"] == 1
        )
        assert import_bridge["category"] == "syntax", import_bridge
        assert "imported module `helper`" in import_bridge["message"], import_bridge

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
        open_root = send_command(proc, "open main")
        assert open_root["ok"] is True, open_root

        status = send_command(proc, "status")
        assert status["ok"] is True, status
        assert status["result"]["diagnosticCount"] >= 1, status

        opened = send_command(proc, "open helper")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(helper_path), opened
        assert opened["result"]["hasAnalysis"] is True, opened

        print_reply = send_command(proc, "pv value")
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


def test_query_open_tracks_new_broken_entry_for_diagnostics(
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
        opened = send_command(proc, "open helper")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(helper_path), opened
        assert str(helper_path) in opened["result"]["entryModules"], opened
        assert opened["result"]["diagnosticCount"] >= 1, opened

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
        helper_open = send_command(proc, "open helper")
        assert helper_open["ok"] is True, helper_open

        root_open = send_command(proc, "open main")
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

        print_renamed = send_command(proc, "pv renamed")
        assert print_renamed["ok"] is True, print_renamed
        assert print_renamed["result"]["item"]["name"] == "renamed", print_renamed

        print_old = send_command(proc, "pv main")
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


def test_query_reload_keeps_broken_compound_assign_ast_alive(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    source_path = app_dir / "terminal.lo"
    source_path.write_text(
        "\n".join(
            [
                "def main() i32 {",
                "    var value i32 = 0",
                "    value += 1",
                "",
                "    if {",
                "",
                "    }",
                "",
                "    ret value",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open terminal")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(source_path), opened
        assert opened["result"]["diagnosticCount"] >= 1, opened

        reload_reply = send_command(proc, "reload")
        assert reload_reply["ok"] is True, reload_reply
        assert reload_reply["result"]["path"] == str(source_path), reload_reply
        assert reload_reply["result"]["diagnosticCount"] >= 1, reload_reply

        diagnostics = send_command(proc, "diagnostics")
        assert diagnostics["ok"] is True, diagnostics
        assert diagnostics["result"]["count"] >= 1, diagnostics

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


def test_query_distinguishes_pv_and_pt_for_same_name_symbols(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "struct Box {",
                "    value i32",
                "}",
                "",
                "def main() i32 {",
                "    var Box = 7",
                "    ret Box",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened

        line = send_command(proc, "goto 6")
        assert line["ok"] is True, line

        print_value = send_command(proc, "pv Box")
        assert print_value["ok"] is True, print_value
        assert print_value["result"]["item"]["kind"] == "local", print_value
        assert print_value["result"]["item"]["name"] == "Box", print_value
        assert print_value["result"]["item"]["type"] == "i32", print_value

        print_type = send_command(proc, "pt Box")
        assert print_type["ok"] is True, print_type
        assert print_type["result"]["item"]["kind"] == "type", print_type
        assert print_type["result"]["item"]["name"] == "Box", print_type

        missing_value = send_command(proc, "pv missing_type_only")
        assert missing_value["ok"] is False, missing_value
        assert "unknown value" in missing_value["result"]["error"], missing_value

        missing_member = send_command(proc, "pv Box.value")
        assert missing_member["ok"] is False, missing_member
        assert "unknown member" in missing_member["result"]["error"], missing_member

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


def test_query_keeps_function_local_scope_on_blank_lines(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "def main() i32 {",
                "",
                "    var x = 1",
                "",
                "    ret x",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened

        line = send_command(proc, "goto 2")
        assert line["ok"] is True, line
        assert line["result"]["hasLocalScope"] is True, line
        assert line["result"]["context"]["kind"] == "func", line
        assert line["result"]["context"]["name"] == "main", line

        locals_on_leading_blank = send_command(proc, "info local")
        assert locals_on_leading_blank["ok"] is True, locals_on_leading_blank
        assert locals_on_leading_blank["result"]["hasLocalScope"] is True, locals_on_leading_blank
        assert locals_on_leading_blank["result"]["count"] == 0, locals_on_leading_blank

        line = send_command(proc, "goto 4")
        assert line["ok"] is True, line
        assert line["result"]["hasLocalScope"] is True, line
        assert line["result"]["context"]["name"] == "main", line

        locals_between_statements = send_command(proc, "info local")
        assert locals_between_statements["ok"] is True, locals_between_statements
        assert locals_between_statements["result"]["hasLocalScope"] is True, locals_between_statements
        assert locals_between_statements["result"]["count"] == 1, locals_between_statements
        assert locals_between_statements["result"]["items"][0]["name"] == "x", locals_between_statements
        assert locals_between_statements["result"]["items"][0]["type"], locals_between_statements

        print_value = send_command(proc, "pv x")
        assert print_value["ok"] is True, print_value
        assert print_value["result"]["item"]["kind"] == "local", print_value
        assert print_value["result"]["item"]["name"] == "x", print_value

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


def test_query_pt_can_print_imported_module_types_and_funcs(
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
                "    ret 0",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "struct Box {",
                "    value i32",
                "}",
                "",
                "def make(seed i32) Box {",
                "    ret Box(value = seed)",
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
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened

        printed_type = send_command(proc, "pt helper.Box")
        assert printed_type["ok"] is True, printed_type
        assert printed_type["result"]["item"]["kind"] == "type", printed_type
        assert printed_type["result"]["item"]["name"] == "Box", printed_type
        members = printed_type["result"]["item"]["typeInfo"]["members"]
        assert members, printed_type
        assert members[0]["name"] == "value", printed_type

        printed_func = send_command(proc, "pt helper.make")
        assert printed_func["ok"] is True, printed_func
        assert printed_func["result"]["item"]["kind"] == "func", printed_func
        assert printed_func["result"]["item"]["name"] == "make", printed_func
        assert "seed: i32" in printed_func["result"]["item"]["signature"], printed_func

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


def test_query_pt_prints_generic_types_like_ordinary_types(
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
                "    ret 0",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    helper_path.write_text(
        "\n".join(
            [
                "struct Box[T] {",
                "    value T",
                "}",
                "",
                "def id[T](value T) T {",
                "    ret value",
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
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened

        printed_type = send_command(proc, "pt helper.Box")
        assert printed_type["ok"] is True, printed_type
        assert printed_type["result"]["item"]["kind"] == "type", printed_type
        assert printed_type["result"]["item"]["name"] == "Box", printed_type
        assert printed_type["result"]["item"]["genericParams"] == [
            {"name": "T", "boundTrait": None}
        ], printed_type
        members = printed_type["result"]["item"]["typeInfo"]["members"]
        assert members, printed_type
        assert members[0]["name"] == "value", printed_type
        assert members[0]["type"] == "T", printed_type

        printed_func = send_command(proc, "pt helper.id")
        assert printed_func["ok"] is True, printed_func
        assert printed_func["result"]["item"]["kind"] == "func", printed_func
        assert printed_func["result"]["item"]["signature"] == "[T](value: T) -> T", printed_func

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


def test_query_pv_tracks_inferred_generic_local_types_in_template_bodies(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "struct Vec[T] {",
                "    value T",
                "}",
                "",
                "def make[T](value T) Vec[T] {",
                "    ret Vec[T](value = value)",
                "}",
                "",
                "def foo[T](input T) {",
                "    var value = make[T](input)",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened
        assert opened["result"]["analyzedFunctionCount"] == 2, opened

        moved = send_command(proc, "goto 10")
        assert moved["ok"] is True, moved
        assert moved["result"]["hasLocalScope"] is True, moved

        locals_reply = send_command(proc, "info local")
        assert locals_reply["ok"] is True, locals_reply
        assert locals_reply["result"]["items"][1]["name"] == "value", locals_reply
        assert locals_reply["result"]["items"][1]["type"] == "Vec[T]", locals_reply

        printed = send_command(proc, "pv value")
        assert printed["ok"] is True, printed
        assert printed["result"]["item"]["kind"] == "local", printed
        assert printed["result"]["item"]["type"] == "Vec[T]", printed
        assert printed["result"]["item"]["typeInfo"]["spelling"] == "Vec[T]", printed

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


def test_query_pv_can_print_object_members_as_values(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "struct Box {",
                "    value i32",
                "}",
                "",
                "def main() i32 {",
                "    var box = Box(value = 7)",
                "    ret box.value",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened

        line = send_command(proc, "goto 6")
        assert line["ok"] is True, line

        printed_member = send_command(proc, "pv box.value")
        assert printed_member["ok"] is True, printed_member
        assert printed_member["result"]["item"]["kind"] == "member", printed_member
        assert printed_member["result"]["item"]["qualifiedName"] == "box.value", printed_member
        assert printed_member["result"]["item"]["detail"] == "field", printed_member
        assert printed_member["result"]["item"]["ownerType"].endswith(
            "Box"
        ), printed_member
        assert printed_member["result"]["item"]["type"] == "i32", printed_member

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


def test_query_pv_can_print_nested_object_member_chains(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "struct Leaf {",
                "    value i32",
                "}",
                "",
                "struct Inner {",
                "    leaf Leaf",
                "}",
                "",
                "struct Outer {",
                "    inner Inner",
                "}",
                "",
                "def main() i32 {",
                "    var box = Outer(inner = Inner(leaf = Leaf(value = 7)))",
                "    ret box.inner.leaf.value",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    proc = subprocess.Popen(
        [str(query_bin), "--format", "json", str(app_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        opened = send_command(proc, "open main")
        assert opened["ok"] is True, opened
        assert opened["result"]["path"] == str(root_path), opened

        line = send_command(proc, "goto 14")
        assert line["ok"] is True, line

        printed_member = send_command(proc, "pv box.inner.leaf.value")
        assert printed_member["ok"] is True, printed_member
        assert printed_member["result"]["item"]["kind"] == "member", printed_member
        assert (
            printed_member["result"]["item"]["qualifiedName"]
            == "box.inner.leaf.value"
        ), printed_member
        assert printed_member["result"]["item"]["owner"] == "box.inner.leaf", printed_member
        assert printed_member["result"]["item"]["type"] == "i32", printed_member

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
