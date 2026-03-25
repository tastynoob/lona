#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path


DEFAULT_SKILL_NAME = "lona-author"
DEFAULT_DESCRIPTION = (
    "Use when the user asks to write, modify, explain, review, build, or run Lona code "
    "and the model should consult the bundled language reference plus runtime command "
    "and build docs."
)
REQUIRED_DOCS = [
    "reference/language/README.md",
    "reference/language/grammar.md",
    "reference/language/expr.md",
    "reference/language/type.md",
    "reference/runtime/commands.md",
    "reference/runtime/native_build.md",
    "reference/runtime/c_ffi.md",
]
LANGUAGE_SOURCE = Path("reference/language")
RUNTIME_DOCS = [
    Path("reference/runtime/commands.md"),
    Path("reference/runtime/native_build.md"),
    Path("reference/runtime/c_ffi.md"),
]


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Build a Codex skill from the repository docs tree."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Repository root. Defaults to the current lona repository.",
    )
    parser.add_argument(
        "--docs-root",
        type=Path,
        default=None,
        help="Docs root to package. Defaults to <repo-root>/docs.",
    )
    parser.add_argument(
        "--skill-name",
        default=DEFAULT_SKILL_NAME,
        help="Generated skill name. Must use lowercase letters, digits, and hyphens.",
    )
    parser.add_argument(
        "--description",
        default=DEFAULT_DESCRIPTION,
        help="Skill description written into SKILL.md frontmatter.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output directory. Defaults to <repo-root>/build/skills/<skill-name>.",
    )
    return parser.parse_args()


def validate_skill_name(skill_name: str) -> None:
    if not re.fullmatch(r"[a-z0-9-]{1,63}", skill_name):
        raise SystemExit(
            "skill name must use only lowercase letters, digits, and hyphens, and be shorter than 64 chars"
        )


def ensure_required_docs(docs_root: Path) -> None:
    missing = [path for path in REQUIRED_DOCS if not (docs_root / path).is_file()]
    if missing:
        joined = "\n".join(f"- {path}" for path in missing)
        raise SystemExit(f"docs tree is missing required files:\n{joined}")


def markdown_title(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("# "):
            return stripped[2:].strip()
    return path.stem


def build_skill_md(skill_name: str, description: str) -> str:
    return f"""---
name: {skill_name}
description: {description}
---

# Lona Author Skill

Use this skill when the task is to write, modify, explain, review, debug, build, or run `lona` code.

## Start Here

1. Read [references/skill-index.md](references/skill-index.md).
2. Load only the references needed for the task:
   - General language syntax and semantics: `references/language/`
   - If the task needs compiler or runner commands: `references/runtime/commands.md`
   - If the task needs entry rules, executable build behavior, or target-mode behavior: `references/runtime/native_build.md`
   - Only if the task touches C ABI boundaries: `references/runtime/c_ffi.md`
3. If behavior is still unclear after reading the bundled references, say that the current skill does not document that behavior clearly instead of guessing.

## Working Rules

- Treat `references/language/` as the primary source of truth.
- Treat `references/runtime/commands.md` as the source of truth for `lona-ir`, `lac`, and `lac-native` usage.
- Treat `references/runtime/native_build.md` as the source of truth for entry rules, hosted vs bare executable behavior, and native build constraints.
- Treat `references/runtime/c_ffi.md` as an extra language-boundary document for `#[extern "C"]`, `#[extern] struct`, and `#[repr "C"]`.
- Prefer the syntax and conventions shown in the bundled references, for example:
  - `cast[T](expr)`
  - `sizeof(expr)` and `sizeof[T]()`
  - explicit pointer spelling such as `T*` and `T[*]`
- When generating new `lona` code, keep examples small, explicit, and consistent with the bundled references.
- Do not infer unsupported language features from compiler internals, old design drafts, or historical plans; those documents are intentionally not bundled into this skill.
- When the bundled references are ambiguous or incomplete, call that out explicitly instead of guessing.

## Output Expectations

- For code generation tasks, produce valid `.lo` code that follows the bundled reference docs.
- For compile or run instructions, use `references/runtime/commands.md` and `references/runtime/native_build.md`.
- For FFI code, follow the restrictions in `references/runtime/c_ffi.md`.
"""


def build_skill_index(references_dir: Path) -> str:
    lines = [
        "# Lona Skill Index",
        "",
        "Read this file first when the skill triggers.",
        "",
        "## Source Priority",
        "",
        "1. `language/`: current stable language syntax and semantics.",
        "2. `runtime/commands.md`: command usage for `lona-ir`, `lac`, and `lac-native`.",
        "3. `runtime/native_build.md`: entry rules, hosted vs bare build behavior, and runtime constraints.",
        "4. `runtime/c_ffi.md`: only for C ABI boundary syntax and restrictions.",
        "",
        "## Recommended Load Order",
        "",
        "- General `.lo` authoring: `language/README.md`, then `grammar.md`, `expr.md`, and `type.md`.",
        "- Functions, structs, variables, and control flow: load the corresponding files under `language/`.",
        "- Command usage and compiler invocation: `runtime/commands.md`.",
        "- Entry rules and executable build behavior: `runtime/native_build.md`.",
        "- C FFI only when needed: `runtime/c_ffi.md`.",
        "",
        "## Included References",
        "",
    ]
    lines.append("### language")
    lines.append("")
    lines.append("Stable language reference for writing Lona code.")
    lines.append("")
    for path in sorted((references_dir / "language").rglob("*.md")):
        rel = path.relative_to(references_dir).as_posix()
        lines.append(f"- [{rel}]({rel}): {markdown_title(path)}")
    lines.append("")
    lines.append("### runtime")
    lines.append("")
    lines.append("Runtime command usage, build behavior, and optional syntax boundary docs for C FFI.")
    lines.append("")
    for path in sorted((references_dir / "runtime").rglob("*.md")):
        rel = path.relative_to(references_dir).as_posix()
        lines.append(f"- [{rel}]({rel}): {markdown_title(path)}")
    lines.append("")
    return "\n".join(lines)


def copy_selected_docs(docs_root: Path, references_dir: Path) -> None:
    shutil.copytree(docs_root / LANGUAGE_SOURCE, references_dir / "language")
    for rel_path in RUNTIME_DOCS:
        src = docs_root / rel_path
        dst = references_dir / "runtime" / src.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def build_skill(repo_root: Path, docs_root: Path, skill_name: str, description: str, output: Path) -> Path:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)

    references_dir = output / "references"
    copy_selected_docs(docs_root, references_dir)
    write_text(output / "SKILL.md", build_skill_md(skill_name, description))
    write_text(references_dir / "skill-index.md", build_skill_index(references_dir))
    return output


def main() -> int:
    args = parse_args()
    validate_skill_name(args.skill_name)

    repo_root = args.repo_root.resolve()
    docs_root = (args.docs_root or (repo_root / "docs")).resolve()
    output = (args.output or (repo_root / "build" / "skills" / args.skill_name)).resolve()

    if not docs_root.is_dir():
        raise SystemExit(f"docs root does not exist: {docs_root}")
    ensure_required_docs(docs_root)

    built = build_skill(repo_root, docs_root, args.skill_name, args.description, output)
    rel = built.relative_to(repo_root) if built.is_relative_to(repo_root) else built
    print(f"generated skill: {rel}")
    print(f"skill entry: {built / 'SKILL.md'}")
    print(f"references: {built / 'references'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
