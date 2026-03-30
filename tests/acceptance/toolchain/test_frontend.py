from __future__ import annotations

from pathlib import Path

from tests.harness import (
    assert_contains,
    assert_magic_bytes,
    assert_not_contains,
    assert_regex,
    nm_contains_symbol,
)
from tests.harness.compiler import CompilerHarness
from tests.harness.compiler import run_command


def test_acceptance_fixture_json(compiler: CompilerHarness, fixtures_dir: Path) -> None:
    result = compiler.emit_json(fixtures_dir / "acceptance_main.lo").expect_ok()
    assert_contains(result.stdout, '"type": "Program"', label="frontend json")
    assert_contains(result.stdout, '"type": "FieldCall"', label="frontend json")


def test_acceptance_fixture_ir_variants(compiler: CompilerHarness, fixtures_dir: Path) -> None:
    base_ir = compiler.emit_ir(fixtures_dir / "acceptance_main.lo").expect_ok().stdout
    assert_regex(base_ir, r"^define i64 @.*Complex\.add\(ptr [^,]+, i64 [^)]+\)", label="frontend ir")
    assert_contains(base_ir, "define i32 @fibo", label="frontend ir")
    assert_not_contains(base_ir, "llvm.dbg.declare", label="frontend ir")

    opt_ir = compiler.emit_ir(fixtures_dir / "acceptance_main.lo", optimize="-O3").expect_ok().stdout
    assert_regex(opt_ir, r"^define i64 @.*Complex\.add\(ptr [^,]+, i64 [^)]+\)", label="optimized ir")
    assert_contains(opt_ir, "define i32 @fibo", label="optimized ir")

    debug_ir = compiler.emit_ir(fixtures_dir / "acceptance_main.lo", debug=True).expect_ok().stdout
    assert_contains(debug_ir, "llvm.dbg.declare", label="debug ir")
    assert_contains(debug_ir, "!llvm.dbg.cu", label="debug ir")
    assert_contains(debug_ir, "!DISubprogram", label="debug ir")


def test_object_emission_marks_native_abi(compiler: CompilerHarness, fixtures_dir: Path) -> None:
    result, obj_path = compiler.emit_obj(fixtures_dir / "acceptance_main.lo", output_name="acceptance_main.o")
    result.expect_ok()
    assert obj_path.stat().st_size > 0, f"expected non-empty object file: {obj_path}"
    assert_magic_bytes(obj_path, b"\x7fELF")
    assert nm_contains_symbol(obj_path, "__lona_native_abi_v0_0", cwd=compiler.repo_root)
    assert_contains(obj_path.read_bytes().decode("latin-1"), "lona.native_abi=v0.0", label="native object payload")


def test_macho_object_emission_keeps_native_abi_payload(compiler: CompilerHarness, fixtures_dir: Path) -> None:
    result, obj_path = compiler.emit_obj(
        fixtures_dir / "acceptance_main.lo",
        output_name="acceptance_main-darwin.o",
        target="x86_64-apple-darwin",
    )
    result.expect_ok()
    assert obj_path.stat().st_size > 0, f"expected non-empty object file: {obj_path}"
    assert_contains(obj_path.read_bytes().decode("latin-1"), "lona.native_abi=v0.0", label="darwin object payload")


def test_bare_targets_emit_only_language_entry(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "entry.lo",
        """
        def run() i32 {
            ret 7
        }

        ret run()
        """,
    )

    for target, expected_triple in [
        ("x86_64-none-elf", 'target triple = "x86_64-none-unknown-elf"'),
        ("x86_64-unknown-none-elf", 'target triple = "x86_64-unknown-none-elf"'),
    ]:
        ir = compiler.emit_ir(input_path, target=target).expect_ok().stdout
        assert_contains(ir, expected_triple, label=f"{target} ir")
        assert_contains(ir, "define i32 @__lona_main__()", label=f"{target} ir")
        assert_contains(ir, "define i32 @run()", label=f"{target} ir")
        assert_not_contains(ir, "define i32 @main()", label=f"{target} ir")
        assert_not_contains(ir, "define i32 @main(i32", label=f"{target} ir")
        assert_not_contains(ir, "@__lona_argc =", label=f"{target} ir")
        assert_not_contains(ir, "@__lona_argv =", label=f"{target} ir")


def test_hosted_target_emits_main_wrapper_and_arg_globals(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "entry.lo",
        """
        def run() i32 {
            ret 7
        }

        ret run()
        """,
    )

    ir = compiler.emit_ir(input_path, target="x86_64-unknown-linux-gnu").expect_ok().stdout
    assert_contains(ir, 'target triple = "x86_64-unknown-linux-gnu"', label="hosted ir")
    assert_contains(ir, "define i32 @__lona_main__()", label="hosted ir")
    assert_contains(ir, "define i32 @run()", label="hosted ir")
    assert_contains(ir, "define i32 @main(i32", label="hosted ir")
    assert_contains(ir, "call i32 @__lona_main__()", label="hosted ir")
    assert_contains(ir, "@__lona_argc =", label="hosted ir")
    assert_contains(ir, "@__lona_argv =", label="hosted ir")


def test_pure_c_abi_object_skips_native_abi_marker(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "c_abi_only.lo",
        """
        #[extern "C"]
        def add(a i32, b i32) i32 {
            ret a + b
        }
        """,
    )
    result, obj_path = compiler.emit_obj(
        input_path,
        output_name="c-abi-only.o",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    assert not nm_contains_symbol(obj_path, "__lona_native_abi_", cwd=compiler.repo_root)
    assert_not_contains(obj_path.read_bytes().decode("latin-1"), "lona.native_abi=", label="c abi object payload")


def test_object_bundle_emits_only_module_objects(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_entry.lo",
        """
        def run() i32 {
            ret 7
        }

        ret run()
        """,
    )

    result, manifest_path = compiler.emit_objects(
        input_path,
        output_name="bundle.manifest",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    manifest = manifest_path.read_text(encoding="utf-8")
    expected_bundle_dir = compiler.repo_root / "lona_cache" / f"{manifest_path.name}.d"
    assert_contains(manifest, "format\tlona-object-bundle-v0", label="object bundle manifest")
    assert_contains(
        manifest,
        "target\tx86_64-unknown-linux-gnu",
        label="object bundle manifest",
    )
    assert_contains(
        manifest,
        "object\tmodule\t",
        label="object bundle manifest",
    )
    assert_not_contains(manifest, "object\thosted-entry\t", label="object bundle manifest")

    object_paths = []
    for line in manifest.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "object":
            object_paths.append(Path(parts[2]))

    assert object_paths, "expected object bundle to list at least one object"
    for object_path in object_paths:
        assert object_path.is_file(), f"expected emitted bundle object: {object_path}"
        assert object_path.stat().st_size > 0, f"expected non-empty bundle object: {object_path}"
        assert object_path.parent == expected_bundle_dir


def test_entry_emission_produces_hosted_wrapper_object(compiler: CompilerHarness, repo_root: Path) -> None:
    result, obj_path = compiler.emit_entry(
        output_name="hosted-entry.o",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    assert obj_path.stat().st_size > 0, f"expected non-empty entry object: {obj_path}"
    symbols = run_command(["nm", "-g", str(obj_path)], cwd=repo_root).stdout
    assert_regex(symbols, r" [TW] main$", label="entry object symbols")
    assert_regex(symbols, r" U __lona_main__$", label="entry object symbols")


def test_object_bundle_member_names_include_compile_profile(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_profile.lo",
        """
        ret 0
        """,
    )
    cache_dir = compiler.output_path("shared-cache")
    linux_result, linux_manifest_path = compiler.emit_objects(
        input_path,
        output_name="linux.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
    )
    linux_result.expect_ok()
    bare_result, bare_manifest_path = compiler.emit_objects(
        input_path,
        output_name="bare.manifest",
        cache_dir=cache_dir,
        target="x86_64-none-elf",
    )
    bare_result.expect_ok()

    def first_module_path(manifest_path: Path) -> Path:
        manifest = manifest_path.read_text(encoding="utf-8")
        for line in manifest.splitlines():
            parts = line.split("\t")
            if len(parts) == 3 and parts[0] == "object" and parts[1] == "module":
                return Path(parts[2])
        raise AssertionError(f"missing module object entry in {manifest_path}")

    assert first_module_path(linux_manifest_path) != first_module_path(bare_manifest_path)


def test_object_bundle_respects_cache_dir_directory(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_cache.lo",
        """
        ret 0
        """,
    )
    cache_dir = compiler.output_path("bundle-cache")
    result, manifest_path = compiler.emit_objects(
        input_path,
        output_name="bundle-cache.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    manifest = manifest_path.read_text(encoding="utf-8")
    expected_cache_bundle_dir = cache_dir / f"{manifest_path.name}.d"
    object_paths = []
    for line in manifest.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "object":
            object_paths.append(Path(parts[2]))

    assert object_paths, "expected emitted cache bundle objects"
    for object_path in object_paths:
        assert object_path.parent == expected_cache_bundle_dir


def test_trait_static_dispatch_lowers_to_direct_method_call(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "trait_static_frontend.lo",
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

        def main() i32 {
            var point = Point(value = 41)
            ret Hash.hash(point)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"%.*Point.*= type \{ i32 \}", label="trait static dispatch ir")
    assert_regex(ir, r"call i32 @.*Point\.hash\(ptr ", label="trait static dispatch ir")
    assert_not_contains(ir, "call i32 %", label="trait static dispatch ir")
    assert_not_contains(ir, "witness", label="trait static dispatch ir")


def test_object_bundle_reuses_cached_objects_across_cli_invocations(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "bundle_reuse.lo",
        """
        ret 0
        """,
    )
    cache_dir = compiler.output_path("reuse-cache")
    first, _ = compiler.emit_objects(
        input_path,
        output_name="reuse.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()
    assert_contains(first.stderr, "compiled-modules: 1", label="first object bundle stats")
    assert_contains(first.stderr, "reused-modules: 0", label="first object bundle stats")
    assert_contains(
        first.stderr,
        "reused-module-objects: 0",
        label="first object bundle stats",
    )

    second, _ = compiler.emit_objects(
        input_path,
        output_name="reuse.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 0", label="second object bundle stats")
    assert_contains(second.stderr, "reused-modules: 1", label="second object bundle stats")
    assert_contains(
        second.stderr,
        "reused-module-objects: 1",
        label="second object bundle stats",
    )


def test_object_bundle_no_cache_forces_full_recompile(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "bundle_no_cache.lo",
        """
        ret 0
        """,
    )
    cache_dir = compiler.output_path("no-cache-reuse")
    first, _ = compiler.emit_objects(
        input_path,
        output_name="no-cache.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()

    second, _ = compiler.emit_objects(
        input_path,
        output_name="no-cache.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
        no_cache=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 1", label="no-cache stats")
    assert_contains(second.stderr, "reused-modules: 0", label="no-cache stats")
    assert_contains(second.stderr, "reused-module-objects: 0", label="no-cache stats")


def test_object_bundle_rejects_lto_mode(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_lto_entry.lo",
        """
        ret 0
        """,
    )
    rejected, _ = compiler.emit_objects(
        input_path,
        output_name="bundle-lto-rejected.manifest",
        target="x86_64-unknown-linux-gnu",
        lto="full",
    )
    rejected.expect_failed()
    assert_contains(rejected.stderr, "does not support `--lto full`", label="object bundle lto")


def test_full_lto_optimizes_linked_modules(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "dep.lo",
        """
        def add1(v i32) i32 {
            ret v + 1
        }
        """,
    )
    app_path = compiler.write_source(
        "app.lo",
        """
        import dep

        def run() i32 {
            ret dep.add1(41)
        }

        ret run()
        """,
    )

    base_ir = compiler.emit_ir(
        app_path,
        optimize="-O3",
        target="x86_64-unknown-linux-gnu",
    ).expect_ok().stdout
    lto_ir = compiler.emit_ir(
        app_path,
        optimize="-O3",
        lto="full",
        target="x86_64-unknown-linux-gnu",
    ).expect_ok().stdout

    assert_regex(base_ir, r"call i32 @.*add1", label="non-lto linked ir")
    assert_contains(lto_ir, "ret i32 42", label="full lto linked ir")
    assert_not_contains(lto_ir, "call i32 @add1", label="full lto linked ir")


def test_missing_return_is_rejected_when_emitting_ir(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "missing_return.lo",
        """
        def bad(a i32) i32 {
            if a < 1 {
                ret 1
            }
        }
        """,
    )
    compiler.emit_ir(input_path).expect_failed()


def test_json_covers_controlflow_and_cast_nodes(compiler: CompilerHarness) -> None:
    controlflow_input = compiler.write_source(
        "json_feature.lo",
        """
        def walk(limit i32) {
            var i i32 = 0
            for i < limit {
                i = i + 1
            }
            ret
        }
        """,
    )
    controlflow_json = compiler.emit_json(controlflow_input).expect_ok().stdout
    assert_contains(controlflow_json, '"type": "Program"', label="controlflow json")
    assert_contains(controlflow_json, '"type": "For"', label="controlflow json")
    assert_contains(controlflow_json, '"cond": {', label="controlflow json")
    assert_contains(controlflow_json, '"body": {', label="controlflow json")
    assert_contains(controlflow_json, '"type": "Return"', label="controlflow json")
    assert_contains(controlflow_json, '"value": null', label="controlflow json")

    cast_input = compiler.write_source(
        "cast_json.lo",
        """
        def widen(v <i32, i32>) <i32, i32> {
            ret cast[<i32, i32>](v)
        }
        """,
    )
    cast_json = compiler.emit_json(cast_input).expect_ok().stdout
    assert_contains(cast_json, '"type": "CastExpr"', label="cast json")
    assert_contains(cast_json, '"targetType": "<i32, i32>"', label="cast json")


def test_single_line_empty_struct_parses_and_lowers(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "empty_struct_single_line.lo",
        """
        struct Empty {}

        def main() i32 {
            var e Empty
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_ok()
    assert_regex(result.stdout, r"%.*Empty = type \{\}", label="empty struct ir")
    assert_contains(result.stdout, "define i32 @main()", label="empty struct ir")


def test_json_preserves_function_pointer_null_and_byte_literals(compiler: CompilerHarness) -> None:
    func_ptr_input = compiler.write_source(
        "func_ptr_json.lo",
        """
        def hold() {
            var slot (i32: i32)*
            var table (: i32* const)[1] const
            ret
        }
        """,
    )
    func_ptr_json = compiler.emit_json(func_ptr_input).expect_ok().stdout
    assert_contains(func_ptr_json, '"declaredType": "(i32: i32)*"', label="function pointer json")
    assert_contains(func_ptr_json, '"declaredType": "(: i32* const)[1] const"', label="function pointer json")

    null_input = compiler.write_source(
        "null_json.lo",
        """
        def main() i32 {
            var ptr i32* = null
            ret 0
        }
        """,
    )
    null_json = compiler.emit_json(null_input).expect_ok().stdout
    assert_contains(null_json, '"declaredType": "i32*"', label="null json")
    assert_contains(null_json, '"value": null', label="null json")

    byte_input = compiler.write_source(
        "byte_json.lo",
        r"""
        def main() {
            var raw = "\xFF\n"
            ret
        }
        """,
    )
    byte_json = compiler.emit_json(byte_input).expect_ok().stdout
    assert_contains(byte_json, r'"value": "\\xFF\\n"', label="byte json")


def test_bool_lowering_uses_i8_storage(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bool.lo",
        """
        def local_bool(a i32) bool {
            var ok bool = true
            if a > 3 {
                ok = false
            }
            if ok {
                ret true
            }
            ret false
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i8 @local_bool", label="bool ir")
    assert_contains(ir, "alloca i8", label="bool ir")
    assert_contains(ir, "store i8 1", label="bool ir")
    assert_contains(ir, "store i8 0", label="bool ir")
    assert_contains(ir, "load i8, ptr ", label="bool ir")
    assert_contains(ir, "ret i8 %", label="bool ir")


def test_pointer_roundtrip_lowering_uses_pointer_alloca(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "pointer.lo",
        """
        def pointer_roundtrip(a i32) i32 {
            var value i32 = a
            var ptr i32* = &value
            *ptr = *ptr + 1
            ret value
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i32 @pointer_roundtrip", label="pointer ir")
    assert_contains(ir, "alloca ptr", label="pointer ir")
    assert_contains(ir, "store ptr ", label="pointer ir")
    assert_contains(ir, "load ptr, ptr ", label="pointer ir")
    assert_contains(ir, "store i32 %", label="pointer ir")
