from __future__ import annotations

import errno
import os
import pty
import re
import select
import subprocess
import time
from pathlib import Path


PROMPT = "lona-query> "
ANSI_ESCAPE_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def _decode_terminal(data: bytes) -> str:
    stripped = ANSI_ESCAPE_RE.sub(b"", data)
    return stripped.replace(b"\r", b"").decode("utf-8", errors="replace")


def _read_until_prompt(master_fd: int, label: str, timeout: float = 5.0) -> str:
    buffer = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        ready, _, _ = select.select([master_fd], [], [], remaining)
        if not ready:
            break
        try:
            chunk = os.read(master_fd, 4096)
        except OSError as exc:
            if exc.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        buffer.extend(chunk)
        text = _decode_terminal(bytes(buffer))
        if text.endswith(PROMPT):
            return text

    text = _decode_terminal(bytes(buffer))
    raise AssertionError(f"timed out waiting for {label}: {text!r}")


def test_query_repl_supports_arrow_editing_and_history(
    query_bin: Path, tmp_path: Path
) -> None:
    app_dir = tmp_path / "app"
    app_dir.mkdir()

    root_path = app_dir / "main.lo"
    root_path.write_text(
        "\n".join(
            [
                "def main() i32 {",
                "    ret 7",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [str(query_bin), str(app_dir)],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        text=False,
        close_fds=True,
    )
    os.close(slave_fd)

    try:
        startup = _read_until_prompt(master_fd, "initial prompt")
        assert "updated root paths" in startup, startup

        os.write(master_fd, b"open ma\x1b[D\x1b[Cin\n")
        opened = _read_until_prompt(master_fd, "open result")
        assert f"module: {root_path}" in opened, opened
        assert "analysis: yes" in opened, opened

        os.write(master_fd, b"status\n")
        status = _read_until_prompt(master_fd, "status result")
        assert f"module: {root_path}" in status, status

        os.write(master_fd, b"\x1b[A\n")
        repeated_status = _read_until_prompt(master_fd, "history status result")
        assert f"module: {root_path}" in repeated_status, repeated_status

        os.write(master_fd, b"quit\n")
        proc.wait(timeout=10)
    finally:
        try:
            os.close(master_fd)
        except OSError:
            pass
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)

    assert proc.returncode == 0, f"unexpected return code {proc.returncode}"
