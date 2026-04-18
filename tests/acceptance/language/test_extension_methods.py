from __future__ import annotations

import re

from tests.acceptance.language._syntax_helpers import _emit_ir, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_extension_methods_support_value_borrowed_and_const_matching(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "extension_value_and_borrowed_ok.lo",
        """
        def i32.kind() i32 {
            ret 1
        }

        def (i32 const*).kind() i32 {
            ret 2
        }

        def (i32 const*).pick() i32 {
            ret 60
        }

        def (i32*).pick() i32 {
            ret 50
        }

        def main() i32 {
            var value i32 = 4
            const frozen i32 = 7
            if 1.kind() != 1 {
                ret 1
            }
            if value.kind() != 1 {
                ret 2
            }
            if value.pick() != 50 {
                ret 3
            }
            if frozen.pick() != 60 {
                ret 4
            }
            ret 0
        }
        """,
    )
    assert_regex(
        ir,
        r"call i32 @.*\.i32\.kind\(i32 ",
        label="extension value receiver ir",
    )
    assert_regex(
        ir,
        r"call i32 @.*\.i32_2a\.pick\(ptr ",
        label="extension mutable borrowed ir",
    )
    assert_regex(
        ir,
        r"call i32 @.*\.i32_20const_2a\.pick\(ptr ",
        label="extension readonly borrowed ir",
    )
    assert re.search(r"call i32 @.*\.i32_20const_2a\.kind\(ptr ", ir) is None, (
        "expected value receiver calls to win over readonly borrowed receivers\n" + ir
    )


def test_extension_methods_allow_struct_temporary_borrowed_receivers(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "extension_struct_temporary_ok.lo",
        """
        struct Pair {
            left i32
            right i32
        }

        def (Pair const*).sum() i32 {
            ret self.left + self.right
        }

        def main() i32 {
            ret Pair(left = 1, right = 2).sum()
        }
        """,
    )
    assert_regex(
        ir,
        r"call i32 @.*\..*Pair_20const_2a\.sum\(ptr ",
        label="extension temporary borrowed ir",
    )


def test_pointer_typed_extension_receivers_do_not_fall_back_to_value_receivers(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "extension_pointer_no_value_fallback.lo",
        """
        def i32.kind() i32 {
            ret 1
        }

        def main() i32 {
            var value i32 = 7
            var ptr i32* = &value
            ret ptr.kind()
        }
        """,
        ["unknown member `i32.kind`"],
    )


def test_scalar_literals_do_not_materialize_borrowed_extension_receivers(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "extension_literal_no_borrowed_materialization.lo",
        """
        def (i32 const*).peek() i32 {
            ret *self
        }

        def main() i32 {
            ret 1.peek()
        }
        """,
        ["unknown member `i32.peek`"],
    )


def test_extension_methods_follow_direct_import_visibility_only(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "extension_import_visibility/dep_a.lo",
        """
        def i32.extra() i32 {
            ret 7
        }
        """,
    )
    compiler.write_source(
        "extension_import_visibility/dep_b.lo",
        """
        import dep_a

        def sentinel() i32 {
            ret 1
        }
        """,
    )

    hidden_main = compiler.write_source(
        "extension_import_visibility/main_hidden.lo",
        """
        import dep_b

        def main() i32 {
            ret 1.extra()
        }
        """,
    )
    hidden = compiler.emit_ir(hidden_main).expect_failed()
    assert_contains(
        hidden.stderr,
        "unknown member `i32.extra`",
        label="extension indirect import diagnostic",
    )

    direct_main = compiler.write_source(
        "extension_import_visibility/main_direct.lo",
        """
        import dep_a

        def main() i32 {
            ret 1.extra()
        }
        """,
    )
    direct_ir = compiler.emit_ir(direct_main).expect_ok().stdout
    assert_regex(
        direct_ir,
        r"call i32 @.*dep_a\.i32\.extra\(i32 ",
        label="extension direct import ir",
    )


def test_extension_method_conflicts_are_reported_for_imports_and_inherent_methods(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "extension_conflicts/dep_a.lo",
        """
        def i32.extra() i32 {
            ret 1
        }
        """,
    )
    compiler.write_source(
        "extension_conflicts/dep_b.lo",
        """
        def i32.extra() i32 {
            ret 2
        }
        """,
    )
    import_conflict = compiler.write_source(
        "extension_conflicts/main_import_conflict.lo",
        """
        import dep_a
        import dep_b

        ret 0
        """,
    )
    import_failed = compiler.emit_ir(import_conflict).expect_failed()
    assert_contains(
        import_failed.stderr,
        "visible extension method conflict for `i32.extra`",
        label="extension import conflict diagnostic",
    )

    _expect_ir_failure(
        compiler,
        "extension_inherent_conflict.lo",
        """
        struct Point {
            x i32

            def len() i32 {
                ret self.x
            }
        }

        def (Point const*).len() i32 {
            ret self.x + 1
        }
        """,
        ["extension method `extension_inherent_conflict.Point.len` conflicts with an inherent method"],
    )


def test_extension_method_bare_selectors_are_rejected(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "extension_selector_bad.lo",
        """
        def i32.kind() i32 {
            ret 1
        }

        def main() i32 {
            1.kind
            ret 0
        }
        """,
        ["extension method `kind` can only be used as a direct call callee"],
    )


def test_extension_methods_do_not_participate_in_generic_bound_lookup(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "extension_generic_bound_lookup_bad.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def (Point const*).extra() i32 {
            ret self.value + 1
        }

        def use[T Hash](value T) i32 {
            ret value.extra()
        }
        """,
        [
            "generic parameter `T` does not provide member `extra` through bound `Hash`",
            "Bounded generic parameters only allow methods provided by bound `Hash`, such as `value.method()`.",
        ],
    )
