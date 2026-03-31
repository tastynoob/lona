from __future__ import annotations

import os
import subprocess
from pathlib import Path
from shutil import which

from tests.harness import assert_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def _detect_cc() -> str:
    if os.environ.get("CC"):
        return os.environ["CC"]
    for candidate in ["cc", "clang-18", "clang"]:
        if which(candidate):
            return candidate
    raise AssertionError("C linker driver not found; set CC or install cc/clang")


def _run_command(cmd: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def _nm_text(path: Path, *args: str, cwd: Path) -> str:
    result = _run_command(["nm", *args, str(path)], cwd=cwd)
    assert result.returncode == 0, result.stderr
    return result.stdout


def test_system_smoke_programs_and_hosted_entry_checks(compiler: CompilerHarness, repo_root: Path) -> None:
    main_program = compiler.write_source(
        "system/return_9.lo",
        """
        def run() i32 {
            ret 9
        }

        ret run()
        """,
    )
    top_level_program = compiler.write_source(
        "system/top_level.lo",
        """
        var x = 1
        x = x + 2
        """,
    )
    lona_import_c_program = compiler.write_source(
        "system/import_abs.lo",
        """
        #[extern "C"]
        def abs(v i32) i32

        def run() i32 {
            ret abs(-9)
        }

        ret run()
        """,
    )
    bad_main_program = compiler.write_source(
        "system/bad_main.lo",
        """
        def main(argc i32) i32 {
            ret argc
        }
        """,
    )
    ffi_main_program = compiler.write_source(
        "system/ffi_main.lo",
        """
        #[extern "C"]
        def main(v i32) i32

        var seed i32 = main(7)
        """,
    )

    build_result, main_exe = compiler.build_system_executable(main_program, output_name="return_9")
    build_result.expect_ok()
    compiler.run_executable(main_exe).expect_exit_code(9)

    lto_build_result, lto_main_exe = compiler.build_system_executable(
        main_program,
        output_name="return_9_lto",
        lto="full",
    )
    lto_build_result.expect_ok()
    compiler.run_executable(lto_main_exe).expect_exit_code(9)

    build_result, top_level_exe = compiler.build_system_executable(top_level_program, output_name="top_level")
    build_result.expect_ok()
    compiler.run_executable(top_level_exe).expect_exit_code(0)

    build_result, import_c_exe = compiler.build_system_executable(lona_import_c_program, output_name="import_abs")
    build_result.expect_ok()
    compiler.run_executable(import_c_exe).expect_exit_code(9)

    bad_main_build, _ = compiler.build_system_executable(bad_main_program, output_name="bad_main")
    bad_main_build.expect_failed()
    assert_contains(bad_main_build.stderr, "cannot build system executable from", label="bad main build")
    assert_contains(
        bad_main_build.stderr,
        "define root-level executable statements in the root module",
        label="bad main build",
    )

    main_obj_result, main_obj = compiler.emit_obj(
        main_program,
        output_name="return_9.o",
        target="x86_64-unknown-linux-gnu",
    )
    main_obj_result.expect_ok()
    defined_symbols = _nm_text(main_obj, "-g", "--defined-only", cwd=repo_root)
    assert_regex(defined_symbols, r" [TW] __lona_main__$", label="main object symbols")
    assert_regex(defined_symbols, r" [TW] main$", label="main object symbols")
    assert_regex(defined_symbols, r" [BD] __lona_argc$", label="main object symbols")
    assert_regex(defined_symbols, r" [BD] __lona_argv$", label="main object symbols")

    ffi_obj_result, ffi_obj = compiler.emit_obj(
        ffi_main_program,
        output_name="ffi_main.o",
        target="x86_64-unknown-linux-gnu",
    )
    ffi_obj_result.expect_ok()
    ffi_defined_symbols = _nm_text(ffi_obj, "-g", "--defined-only", cwd=repo_root)
    ffi_all_symbols = _nm_text(ffi_obj, "-g", cwd=repo_root)
    assert_regex(ffi_defined_symbols, r" [TW] __lona_main__$", label="ffi main object symbols")
    assert_regex(ffi_all_symbols, r" U main$", label="ffi main object symbols")

    ffi_build, _ = compiler.build_system_executable(ffi_main_program, output_name="ffi_main")
    ffi_build.expect_failed()
    assert_contains(
        ffi_build.stderr,
        "already declares or imports a non-entry symbol named `main`",
        label="ffi main build",
    )


def test_system_trait_dyn_dispatch_runtime(compiler: CompilerHarness) -> None:
    program = compiler.write_source(
        "system/trait_dyn.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: Hash

        var point = Point(value = 41)
        var h Hash dyn = cast[Hash dyn](&point)
        ret h.hash()
        """,
    )

    build_result, exe = compiler.build_system_executable(
        program,
        output_name="trait_dyn",
    )
    build_result.expect_ok()
    compiler.run_executable(exe).expect_exit_code(42)


def test_system_trait_dyn_dispatch_runtime_with_indirect_result_aggregate(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "system/trait_dyn_big_dep.lo",
        """
        struct Big {
            a i64
            b i64
            c i64
        }

        trait Factory {
            def make() Big
        }

        struct Maker {
            seed i64

            def make() Big {
                ret Big(a = self.seed, b = self.seed + 1, c = self.seed + 2)
            }
        }

        impl Maker: Factory
        """,
    )
    program = compiler.write_source(
        "system/trait_dyn_big_main.lo",
        """
        import trait_dyn_big_dep

        var maker = trait_dyn_big_dep.Maker(seed = 40)
        var factory trait_dyn_big_dep.Factory dyn =
            cast[trait_dyn_big_dep.Factory dyn](&maker)
        var big = factory.make()
        if big.c == 42 {
            ret 42
        }
        ret 1
        """,
    )

    build_result, exe = compiler.build_system_executable(
        program,
        output_name="trait_dyn_big",
    )
    build_result.expect_ok()
    compiler.run_executable(exe).expect_exit_code(42)


def test_system_smoke_c_abi_interop_and_examples(compiler: CompilerHarness, repo_root: Path) -> None:
    cc_bin = _detect_cc()

    export_add_program = compiler.write_source(
        "system/export_add.lo",
        """
        #[extern "C"]
        def lona_add(a i32, b i32) i32 {
            ret a + b
        }
        """,
    )
    export_add_harness = compiler.write_source(
        "system/export_add_harness.c",
        """
        extern int lona_add(int a, int b);

        int main(void) {
            return lona_add(4, 5);
        }
        """,
    )
    export_add_ir = compiler.output_path("export_add.ll")
    export_add_obj = compiler.output_path("export_add.o")
    compile_result = _run_command(
        [
            "python3",
            str(repo_root / "tests" / "tools" / "compile_case.py"),
            str(export_add_program),
            str(export_add_ir),
            str(export_add_obj),
        ],
        cwd=repo_root,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    export_add_exe = compiler.output_path("export_add_harness")
    cc_result = _run_command(
        [cc_bin, "-Werror", str(export_add_harness), str(export_add_obj), "-o", str(export_add_exe)],
        cwd=repo_root,
    )
    assert cc_result.returncode == 0, cc_result.stderr
    compiler.run_executable(export_add_exe).expect_exit_code(9)

    repr_point_program = compiler.write_source(
        "system/repr_point.lo",
        """
        #[repr "C"]
        struct Point {
            set x i32
            set y i32
        }

        #[extern "C"]
        def shift(p Point*) i32 {
            (*p).x = (*p).x + 3
            (*p).y = (*p).y + 4
            ret (*p).x + (*p).y
        }
        """,
    )
    repr_point_harness = compiler.write_source(
        "system/repr_point_harness.c",
        """
        struct Point {
            int x;
            int y;
        };

        extern int shift(struct Point *p);

        int main(void) {
            struct Point p = {1, 2};
            int sum = shift(&p);
            return (sum == 10 && p.x == 4 && p.y == 6) ? 0 : 1;
        }
        """,
    )
    repr_point_ir = compiler.output_path("repr_point.ll")
    repr_point_obj = compiler.output_path("repr_point.o")
    compile_result = _run_command(
        [
            "python3",
            str(repo_root / "tests" / "tools" / "compile_case.py"),
            str(repr_point_program),
            str(repr_point_ir),
            str(repr_point_obj),
        ],
        cwd=repo_root,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    repr_point_exe = compiler.output_path("repr_point_harness")
    cc_result = _run_command(
        [cc_bin, "-Werror", str(repr_point_harness), str(repr_point_obj), "-o", str(repr_point_exe)],
        cwd=repo_root,
    )
    assert cc_result.returncode == 0, cc_result.stderr
    compiler.run_executable(repr_point_exe).expect_exit_code(0)

    list_crud_program = compiler.write_source(
        "system/list_crud.lo",
        """
        #[repr "C"]
        struct Slot {
            set used i32
            set value i32
            set next i32
        }

        #[extern "C"]
        def malloc(size u64) Slot*

        #[extern "C"]
        def free(p Slot*)

        #[extern "C"]
        def puts(msg i8*) i32

        def list_init(slots Slot[*], ref free_head i32, capacity i32) {
            free_head = 0
            var i i32 = 0
            for i < capacity {
                slots(i).used = 0
                slots(i).value = 0
                if i + 1 < capacity {
                    slots(i).next = i + 1
                } else {
                    slots(i).next = -1
                }
                i = i + 1
            }
        }

        def take_slot(slots Slot[*], ref free_head i32) i32 {
            if free_head < 0 {
                ret -1
            }
            var idx i32 = free_head
            free_head = slots(idx).next
            slots(idx).used = 1
            slots(idx).next = -1
            ret idx
        }

        def release_slot(slots Slot[*], ref free_head i32, idx i32) {
            slots(idx).used = 0
            slots(idx).value = 0
            slots(idx).next = free_head
            free_head = idx
        }

        def list_append(slots Slot[*], ref head i32, ref free_head i32, value i32) i32 {
            var idx i32 = take_slot(slots, ref free_head)
            if idx < 0 {
                ret -1
            }
            slots(idx).value = value
            slots(idx).next = -1
            if head < 0 {
                head = idx
                ret idx
            }
            var cur i32 = head
            for slots(cur).next >= 0 {
                cur = slots(cur).next
            }
            slots(cur).next = idx
            ret idx
        }

        def list_find(slots Slot[*], head i32, value i32) i32 {
            var cur i32 = head
            for cur >= 0 {
                if slots(cur).used != 0 && slots(cur).value == value {
                    ret cur
                }
                cur = slots(cur).next
            }
            ret -1
        }

        def list_update(slots Slot[*], head i32, old_value i32, new_value i32) i32 {
            var idx i32 = list_find(slots, head, old_value)
            if idx < 0 {
                ret 0
            }
            slots(idx).value = new_value
            ret 1
        }

        def list_remove(slots Slot[*], ref head i32, ref free_head i32, value i32) i32 {
            var prev i32 = -1
            var cur i32 = head
            for cur >= 0 {
                if slots(cur).used != 0 && slots(cur).value == value {
                    var next i32 = slots(cur).next
                    if prev < 0 {
                        head = next
                    } else {
                        slots(prev).next = next
                    }
                    release_slot(slots, ref free_head, cur)
                    ret 1
                }
                prev = cur
                cur = slots(cur).next
            }
            ret 0
        }

        def list_len(slots Slot[*], head i32) i32 {
            var cur i32 = head
            var count i32 = 0
            for cur >= 0 {
                if slots(cur).used != 0 {
                    count = count + 1
                }
                cur = slots(cur).next
            }
            ret count
        }

        def list_sum(slots Slot[*], head i32) i32 {
            var cur i32 = head
            var total i32 = 0
            for cur >= 0 {
                if slots(cur).used != 0 {
                    total = total + slots(cur).value
                }
                cur = slots(cur).next
            }
            ret total
        }

        def run() i32 {
            var slots Slot[*] = malloc(64)
            var head i32 = -1
            var free_head i32 = -1
            var status i32 = 0

            list_init(slots, ref free_head, 4)

            if list_append(slots, ref head, ref free_head, 10) < 0 {
                status = 11
            }
            if status == 0 && (list_append(slots, ref head, ref free_head, 20) < 0) {
                status = 12
            }
            if status == 0 && (list_append(slots, ref head, ref free_head, 30) < 0) {
                status = 13
            }
            if status == 0 && (list_len(slots, head) != 3) {
                status = 14
            }
            if status == 0 && (list_find(slots, head, 20) < 0) {
                status = 15
            }
            if status == 0 && (list_update(slots, head, 20, 25) == 0) {
                status = 16
            }
            if status == 0 && (list_find(slots, head, 25) < 0) {
                status = 17
            }
            if status == 0 && (list_remove(slots, ref head, ref free_head, 10) == 0) {
                status = 18
            }
            if status == 0 && (list_find(slots, head, 10) >= 0) {
                status = 19
            }
            if status == 0 && (list_append(slots, ref head, ref free_head, 40) < 0) {
                status = 20
            }
            if status == 0 && (list_len(slots, head) != 3) {
                status = 21
            }
            if status == 0 && (list_sum(slots, head) != 95) {
                status = 22
            }

            if status == 0 {
                var msg i8[8] = {108, 105, 115, 116, 32, 111, 107, 0}
                puts(&msg(0))
            }

            free(slots)
            ret status
        }

        ret run()
        """,
    )
    build_result, list_crud_exe = compiler.build_system_executable(list_crud_program, output_name="list_crud")
    build_result.expect_ok()
    compiler.run_executable(list_crud_exe).expect_exit_code(0)

    linked_list_program = repo_root / "example" / "c_ffi_linked_list.lo"
    build_result, linked_list_exe = compiler.build_system_executable(linked_list_program, output_name="linked_list")
    build_result.expect_ok()
    compiler.run_executable(linked_list_exe).expect_exit_code(0)
