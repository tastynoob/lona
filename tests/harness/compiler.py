from __future__ import annotations

import os
import shlex
import subprocess
import textwrap
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CommandResult:
    cmd: list[str]
    cwd: Path
    returncode: int
    stdout: str
    stderr: str

    def describe(self) -> str:
        stdout = self.stdout.strip()
        stderr = self.stderr.strip()
        parts = [
            f"cwd: {self.cwd}",
            f"cmd: {' '.join(shlex.quote(part) for part in self.cmd)}",
            f"exit: {self.returncode}",
            f"stdout:\n{textwrap.indent(stdout or '<empty>', '  ')}",
            f"stderr:\n{textwrap.indent(stderr or '<empty>', '  ')}",
        ]
        return "\n".join(parts)

    def expect_ok(self) -> "CommandResult":
        assert self.returncode == 0, f"command failed unexpectedly\n{self.describe()}"
        return self

    def expect_failed(self) -> "CommandResult":
        assert self.returncode != 0, f"command unexpectedly succeeded\n{self.describe()}"
        return self


@dataclass
class ExecutableResult:
    path: Path
    returncode: int
    stdout: str
    stderr: str

    def describe(self) -> str:
        stdout = self.stdout.strip()
        stderr = self.stderr.strip()
        parts = [
            f"path: {self.path}",
            f"exit: {self.returncode}",
            f"stdout:\n{textwrap.indent(stdout or '<empty>', '  ')}",
            f"stderr:\n{textwrap.indent(stderr or '<empty>', '  ')}",
        ]
        return "\n".join(parts)

    def expect_exit_code(self, expected: int) -> "ExecutableResult":
        assert self.returncode == expected, (
            f"unexpected executable exit code: expected {expected}, got {self.returncode}\n{self.describe()}"
        )
        return self


def run_command(cmd: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> CommandResult:
    completed = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return CommandResult(
        cmd=cmd,
        cwd=cwd,
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
    )


def nm_contains_symbol(path: Path, symbol: str, *, cwd: Path) -> bool:
    result = run_command(["nm", "-a", str(path)], cwd=cwd)
    result.expect_ok()
    return symbol in result.stdout


class CompilerHarness:
    def __init__(
        self,
        *,
        repo_root: Path,
        compiler_bin: Path,
        system_driver: Path,
        native_driver: Path,
        tmp_path: Path,
    ):
        self.repo_root = repo_root
        self.compiler_bin = compiler_bin
        self.system_driver = system_driver
        self.native_driver = native_driver
        self.tmp_path = tmp_path

    def write_source(self, name: str, content: str) -> Path:
        path = self.tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content).lstrip("\n"), encoding="utf-8")
        return path

    def output_path(self, name: str) -> Path:
        path = self.tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def emit_json(self, input_path: Path) -> CommandResult:
        return self._run([str(input_path)])

    def emit_ir(
        self,
        input_path: Path,
        *,
        verify_ir: bool = True,
        target: str | None = None,
        optimize: str | None = None,
        debug: bool = False,
        stats: bool = False,
    ) -> CommandResult:
        args = ["--emit", "ir"]
        if verify_ir:
            args.append("--verify-ir")
        if target is not None:
            args.extend(["--target", target])
        if optimize is not None:
            args.append(optimize)
        if debug:
            args.append("-g")
        if stats:
            args.append("--stats")
        args.append(str(input_path))
        return self._run(args)

    def emit_obj(
        self,
        input_path: Path,
        *,
        output_name: str,
        verify_ir: bool = True,
        target: str | None = None,
    ) -> tuple[CommandResult, Path]:
        output_path = self.output_path(output_name)
        args = ["--emit", "obj"]
        if verify_ir:
            args.append("--verify-ir")
        if target is not None:
            args.extend(["--target", target])
        args.extend([str(input_path), str(output_path)])
        return self._run(args), output_path

    def build_system_executable(self, input_path: Path, *, output_name: str) -> tuple[CommandResult, Path]:
        output_path = self.output_path(output_name)
        env = os.environ.copy()
        env["LC_ALL"] = "C"
        result = run_command(
            [str(self.system_driver), str(input_path), str(output_path)],
            cwd=self.repo_root,
            env=env,
        )
        return result, output_path

    def build_native_executable(self, input_path: Path, *, output_name: str) -> tuple[CommandResult, Path]:
        output_path = self.output_path(output_name)
        env = os.environ.copy()
        env["LC_ALL"] = "C"
        result = run_command(
            [str(self.native_driver), str(input_path), str(output_path)],
            cwd=self.repo_root,
            env=env,
        )
        return result, output_path

    def run_executable(self, path: Path) -> ExecutableResult:
        completed = subprocess.run(
            [str(path)],
            cwd=self.repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        return ExecutableResult(
            path=path,
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
        )

    def _run(self, args: list[str]) -> CommandResult:
        env = os.environ.copy()
        env["LC_ALL"] = "C"
        return run_command([str(self.compiler_bin), *args], cwd=self.repo_root, env=env)
