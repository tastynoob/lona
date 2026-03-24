from __future__ import annotations

from tests.harness.compiler import CompilerHarness


def test_native_smoke_programs_run(compiler: CompilerHarness) -> None:
    cases = [
        (
            "return_42.lo",
            """
            def run() i32 {
                ret 42
            }

            ret run()
            """,
            42,
        ),
        (
            "top_level.lo",
            """
            var x = 1
            x = x + 1
            """,
            0,
        ),
        (
            "ref_local_address.lo",
            """
            def run() i32 {
                var x i32 = 1
                ref alias i32 = x
                var p i32* = &alias
                *p = 11
                ret x
            }

            ret run()
            """,
            11,
        ),
        (
            "ref_param_address.lo",
            """
            def poke(ref x i32) i32 {
                var p i32* = &x
                *p = 9
                ret x
            }

            def run() i32 {
                var x i32 = 1
                ret poke(ref x)
            }

            ret run()
            """,
            9,
        ),
        (
            "array_pointer.lo",
            """
            def run() i32 {
                var row i32[4] = {1, 2, 3, 4}
                var p i32[4]* = &row
                (*p)(2) = 13
                ret row(2)
            }

            ret run()
            """,
            13,
        ),
        (
            "method_self.lo",
            """
            struct Counter {
                value i32

                def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            def run() i32 {
                var c Counter
                c.value = 2
                c.bump(3)
                ret c.value
            }

            ret run()
            """,
            5,
        ),
        (
            "method_temp.lo",
            """
            struct Counter {
                value i32

                def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            def run() i32 {
                ret Counter(1).bump(2)
            }

            ret run()
            """,
            3,
        ),
        (
            "struct_fields.lo",
            """
            def inc(v i32) i32 {
                ret v + 1
            }

            struct Mixed {
                flag bool
                ratio f32
                bits u8[4]
                pair <i32, bool>
                ptr i32*
                cb (i32: i32)
            }

            def run() i32 {
                var x i32 = 41
                var raw u8[4] = cast[f32](1).tobits()
                var pair <i32, bool> = (1, true)
                var mixed = Mixed(flag = true, ratio = cast[f32](1), bits = raw, pair = pair, ptr = &x, cb = inc&<i32>)
                if mixed.flag && mixed.pair._2 && (mixed.ratio >= cast[f32](1)) {
                    mixed.bits(0) = 1
                    ret mixed.cb(*mixed.ptr) + mixed.bits(0) + mixed.pair._1
                }
                ret 0
            }

            ret run()
            """,
            44,
        ),
        (
            "small_struct_packed.lo",
            """
            struct Pair {
                left i32
                right i32

                def swap(extra i32) Pair {
                    var out Pair
                    out.left = self.right + extra
                    out.right = self.left + extra
                    ret out
                }
            }

            def echo(v Pair) Pair {
                ret v
            }

            def run() i32 {
                var p = Pair(left = 1, right = 2)
                var out = echo(p).swap(3)
                ret out.left + out.right
            }

            ret run()
            """,
            9,
        ),
        (
            "medium_struct_direct_return.lo",
            """
            struct Triple {
                a i32
                b i32
                c i32

                def shift(delta i32) Triple {
                    var out Triple
                    out.a = self.b + delta
                    out.b = self.c + delta
                    out.c = self.a + delta
                    ret out
                }
            }

            def echo(v Triple) Triple {
                ret v
            }

            def run() i32 {
                var triple = Triple(a = 1, b = 2, c = 3)
                var out = echo(triple).shift(4)
                ret out.a + out.b + out.c
            }

            ret run()
            """,
            18,
        ),
    ]

    for name, source, expected_exit in cases:
        input_path = compiler.write_source(f"native/{name}", source)
        build_result, exe_path = compiler.build_native_executable(input_path, output_name=f"native-{name}.bin")
        build_result.expect_ok()
        compiler.run_executable(exe_path).expect_exit_code(expected_exit)

    lto_input = compiler.write_source(
        "native/return_13_lto.lo",
        """
        def run() i32 {
            ret 13
        }

        ret run()
        """,
    )
    lto_build_result, lto_exe_path = compiler.build_native_executable(
        lto_input,
        output_name="native-return_13_lto.bin",
        lto="full",
    )
    lto_build_result.expect_ok()
    compiler.run_executable(lto_exe_path).expect_exit_code(13)
