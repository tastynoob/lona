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


def test_compiler_version_prints_language_and_revision(compiler: CompilerHarness) -> None:
    result = run_command([str(compiler.compiler_bin), "--version"], cwd=compiler.repo_root)
    result.expect_ok()
    assert_regex(
        result.stdout,
        r"^0\.1 beta \+ (?:[0-9a-f]{12}|unknown)\n?$",
        label="compiler version",
    )
    assert result.stderr == "", result.describe()


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
    result, obj_path = compiler.emit_linked_obj(
        fixtures_dir / "acceptance_main.lo", output_name="acceptance_main.o"
    )
    result.expect_ok()
    assert obj_path.stat().st_size > 0, f"expected non-empty object file: {obj_path}"
    assert_magic_bytes(obj_path, b"\x7fELF")
    assert nm_contains_symbol(obj_path, "__lona_native_abi_v0_0", cwd=compiler.repo_root)
    assert_contains(obj_path.read_bytes().decode("latin-1"), "lona.native_abi=v0.0", label="native object payload")


def test_macho_object_emission_keeps_native_abi_payload(compiler: CompilerHarness, fixtures_dir: Path) -> None:
    result, obj_path = compiler.emit_linked_obj(
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
    result, obj_path = compiler.emit_linked_obj(
        input_path,
        output_name="c-abi-only.o",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    assert not nm_contains_symbol(obj_path, "__lona_native_abi_", cwd=compiler.repo_root)
    assert_not_contains(obj_path.read_bytes().decode("latin-1"), "lona.native_abi=", label="c abi object payload")


def test_linked_object_reuses_default_bitcode_cache(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "linked_object_cache/dep.lo",
        """
        def add1(v i32) i32 {
            ret v + 1
        }
        """,
    )
    app_path = compiler.write_source(
        "linked_object_cache/main.lo",
        """
        import dep

        def run() i32 {
            ret dep.add1(41)
        }

        ret run()
        """,
    )

    first, _ = compiler.emit_linked_obj(
        app_path,
        output_name="linked-default-cache.o",
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()
    assert_contains(first.stderr, "compiled-modules: 2", label="linked object default cache first stats")
    assert_contains(first.stderr, "reused-modules: 0", label="linked object default cache first stats")
    assert_contains(
        first.stderr,
        "reused-module-bitcode: 0",
        label="linked object default cache first stats",
    )

    cache_dir = compiler.output_path("linked-default-cache.o.d")
    assert cache_dir.is_dir(), f"expected linked-object bitcode cache dir: {cache_dir}"
    assert len(list(cache_dir.glob("*.bc"))) == 2, f"expected cached module bitcode in {cache_dir}"

    second, _ = compiler.emit_linked_obj(
        app_path,
        output_name="linked-default-cache.o",
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 0", label="linked object default cache reuse stats")
    assert_contains(second.stderr, "reused-modules: 2", label="linked object default cache reuse stats")
    assert_contains(
        second.stderr,
        "reused-module-bitcode: 2",
        label="linked object default cache reuse stats",
    )
    assert_contains(
        second.stderr,
        "emitted-module-bitcode: 0",
        label="linked object default cache reuse stats",
    )


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

    result, manifest_path = compiler.emit_obj_bundle(
        input_path,
        output_name="bundle.manifest",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    manifest = manifest_path.read_text(encoding="utf-8")
    expected_bundle_dir = compiler.repo_root / "lona_cache" / f"{manifest_path.name}.d"
    assert_contains(manifest, "format\tlona-artifact-bundle-v1", label="object bundle manifest")
    assert_contains(manifest, "kind\tobj", label="object bundle manifest")
    assert_contains(
        manifest,
        "target\tx86_64-unknown-linux-gnu",
        label="object bundle manifest",
    )
    assert_contains(
        manifest,
        "artifact\tobj\t",
        label="object bundle manifest",
    )
    assert_not_contains(manifest, "artifact\tobj\thosted-entry\t", label="object bundle manifest")

    object_paths = []
    for line in manifest.splitlines():
        parts = line.split("\t")
        if len(parts) == 4 and parts[0] == "artifact" and parts[1] == "obj":
            object_paths.append(Path(parts[3]))

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
    linux_result, linux_manifest_path = compiler.emit_obj_bundle(
        input_path,
        output_name="linux.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
    )
    linux_result.expect_ok()
    bare_result, bare_manifest_path = compiler.emit_obj_bundle(
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
            if len(parts) == 4 and parts[0] == "artifact" and parts[1] == "obj":
                return Path(parts[3])
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
    result, manifest_path = compiler.emit_obj_bundle(
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
        if len(parts) == 4 and parts[0] == "artifact" and parts[1] == "obj":
            object_paths.append(Path(parts[3]))

    assert object_paths, "expected emitted cache bundle objects"
    for object_path in object_paths:
        assert object_path.parent == expected_cache_bundle_dir


def test_generic_v0_any_pointer_casts_lower_to_plain_ptr_storage(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_any_ptr_frontend.lo",
        """
        def main() usize {
            var value i32 = 1
            var typed i32* = &value
            var erased any* = cast[any*](typed)
            var readonly any const* = cast[any const*](typed)
            ret sizeof[any*]() + sizeof[any const*]()
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i64 @main()", label="generic any ptr ir")
    assert_contains(ir, "alloca ptr, align 8", label="generic any ptr ir")
    assert_contains(ir, "store ptr", label="generic any ptr ir")
    assert_not_contains(ir, "%any", label="generic any ptr ir")


def test_generic_v0_any_pointer_interface_decls_lower_cleanly(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_any_ptr_interface.lo",
        """
        struct Holder {
            ptr any*
            readonly any const*
        }

        global hold any* = null

        def erase(ptr any*) any* {
            ret ptr
        }

        def erase_ro(ptr any const*) any const* {
            ret ptr
        }

        def main() any* {
            var value i32 = 1
            var typed i32* = &value
            hold = cast[any*](typed)
            var holder Holder = Holder(ptr = hold, readonly = cast[any const*](typed))
            ret erase(holder.ptr)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "Holder = type { ptr, ptr }", label="generic any ptr interface ir")
    assert_contains(ir, "@hold = global ptr null", label="generic any ptr interface ir")
    assert_contains(ir, "define ptr @erase(ptr", label="generic any ptr interface ir")
    assert_contains(ir, "define ptr @erase_ro(ptr", label="generic any ptr interface ir")
    assert_contains(ir, "define ptr @main()", label="generic any ptr interface ir")
    assert_not_contains(ir, "%any", label="generic any ptr interface ir")


def test_generic_v0_templates_do_not_emit_runtime_symbols_before_instantiation(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_templates_no_runtime_symbols.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }

        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i32 @main()", label="generic template ir")
    assert_not_contains(ir, "define i32 @id(", label="generic template ir")
    assert_not_contains(ir, "@Box.get", label="generic template ir")


def test_generic_v0_applied_type_pointers_form_concrete_runtime_identities(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_applied_type_pointer_round4.lo",
        """
        struct Box[T] {
            value T
        }

        def main() Box[i32]* {
            ret null
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define ptr @main()", label="generic applied ptr ir")


def test_generic_v0_same_module_applied_structs_emit_concrete_runtime_layout_and_methods(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_applied_type_runtime_round3.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }

        def main() i32 {
            var box Box[i32] = Box[i32](value = 5)
            ret box.get()
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i32 @main()", label="generic applied value ir")
    assert_contains(
        ir,
        '%"Box[i32]" = type { i32 }',
        label="generic applied value ir",
    )
    assert_contains(
        ir,
        "@generic_5fapplied_5ftype_5fruntime_5fround3_2eBox_5bi32_5d.get",
        label="generic applied value ir",
    )


def test_generic_v0_same_module_calls_emit_concrete_runtime_symbols(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_same_module_runtime_symbols_round0.lo",
        """
        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            var left i32 = id[i32](1)
            var right i32 = id(2)
            ret left + right
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(
        ir,
        "define i32 @generic_same_module_runtime_symbols_round0.id__inst__i32",
        label="generic same-module ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_same_module_runtime_symbols_round0.id__inst__i32(i32 1)",
        label="generic same-module ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_same_module_runtime_symbols_round0.id__inst__i32(i32 2)",
        label="generic same-module ir",
    )
    assert_not_contains(
        ir,
        "generic function instantiation is not implemented yet",
        label="generic same-module ir",
    )


def test_generic_v0_specialized_function_refs_lower_to_concrete_symbols(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "generic_specialized_function_ref_round0.lo",
        """
        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            var cb (i32: i32) = @id[i32]
            ret cb(3)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(
        ir,
        "store ptr @generic_specialized_function_ref_round0.id__inst__i32",
        label="generic function ref ir",
    )
    assert_contains(ir, "call i32 %2(i32 3)", label="generic function ref ir")


def test_generic_v0_imported_calls_and_refs_emit_concrete_runtime_symbols(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_imported_runtime_symbols/dep.lo",
        """
        def id[T](value T) T {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_imported_runtime_symbols/main.lo",
        """
        import dep

        def main() i32 {
            var cb (i32: i32) = @dep.id[i32]
            var left i32 = dep.id[i32](1)
            var right i32 = cb(2)
            ret left + right
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(
        ir,
        "define i32 @dep.id__inst__i32",
        label="generic imported function ir",
    )
    assert_contains(
        ir,
        "store ptr @dep.id__inst__i32",
        label="generic imported function ir",
    )
    assert_contains(
        ir,
        "call i32 @dep.id__inst__i32(i32 1)",
        label="generic imported function ir",
    )


def test_generic_v0_imported_applied_structs_emit_concrete_layout_and_methods(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_imported_struct_runtime/dep.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_imported_struct_runtime/main.lo",
        """
        import dep

        def main() i32 {
            var box dep.Box[i32] = dep.Box[i32](value = 5)
            ret box.get()
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(
        ir,
        '%"dep.Box[i32]" = type { i32 }',
        label="generic imported struct ir",
    )
    assert_contains(
        ir,
        "@dep_2eBox_5bi32_5d.get",
        label="generic imported struct ir",
    )


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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var ptr Point* = &point
            ret Hash.hash(ptr)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"%.*Point.*= type \{ i32 \}", label="trait static dispatch ir")
    assert_regex(ir, r"call i32 @.*Point\.hash\(ptr ", label="trait static dispatch ir")
    assert_not_contains(ir, "call i32 %", label="trait static dispatch ir")
    assert_not_contains(ir, "witness", label="trait static dispatch ir")


def test_trait_dyn_dispatch_lowers_to_witness_indirection_without_struct_vptrs(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "trait_dyn_frontend.lo",
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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var h Hash dyn = cast[Hash dyn](&point)
            ret h.hash()
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"%.*Point = type \{ i32 \}", label="trait dyn dispatch ir")
    assert_contains(ir, "@__lona_trait_witness__", label="trait dyn dispatch ir")
    assert_regex(
        ir,
        r"@__lona_trait_witness__.* = internal constant \[1 x ptr\] \[ptr @.*Point\.__trait__\..*Hash\.hash\]",
        label="trait dyn dispatch ir",
    )
    assert_contains(
        ir,
        "getelementptr inbounds [1 x ptr], ptr %trait.witness",
        label="trait dyn dispatch ir",
    )
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait dyn dispatch ir",
    )
    assert_not_contains(ir, "type { ptr, i32 }", label="trait dyn dispatch ir")


def test_trait_dyn_indirect_results_keep_self_before_sret_in_witness_calls(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "trait_dyn_indirect_result.lo",
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

        impl Factory for Maker {
            def make() Big {
                ret self.make()
            }
        }

        def main() i32 {
            var maker = Maker(seed = 40)
            var f Factory dyn = cast[Factory dyn](&maker)
            var big = f.make()
            if big.c == 42 {
                ret 42
            }
            ret 1
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"%.*Big = type \{ i64, i64, i64 \}", label="trait dyn indirect result ir")
    assert_regex(
        ir,
        r"^define void @.*Maker\.make\(ptr [^,]+, ptr [^)]+\)",
        label="trait dyn indirect result ir",
    )
    assert_regex(
        ir,
        r"call void %trait\.slot\(ptr %trait\.data, ptr [^)]+\)",
        label="trait dyn indirect result ir",
    )


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
    first, _ = compiler.emit_obj_bundle(
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

    second, _ = compiler.emit_obj_bundle(
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
    first, _ = compiler.emit_obj_bundle(
        input_path,
        output_name="no-cache.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()

    second, _ = compiler.emit_obj_bundle(
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


def test_object_bundle_invalidates_imported_generic_function_instances_when_owner_body_changes(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "bundle_import_generic_function/dep.lo",
        """
        def size_of_value[T](value T) usize {
            ret sizeof[T]()
        }

        def make() i32 {
            ret 7
        }
        """,
    )
    main_path = compiler.write_source(
        "bundle_import_generic_function/main.lo",
        """
        import dep

        def main() usize {
            ret dep.size_of_value(dep.make())
        }
        """,
    )
    cache_dir = compiler.output_path("import-generic-function-cache")

    first, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-function.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()
    assert_contains(first.stderr, "compiled-modules: 2", label="generic function body change first stats")
    assert_contains(first.stderr, "reused-modules: 0", label="generic function body change first stats")
    assert_contains(
        first.stderr,
        "reused-module-objects: 0",
        label="generic function body change first stats",
    )

    compiler.write_source(
        "bundle_import_generic_function/dep.lo",
        """
        def size_of_value[T](value T) usize {
            ret sizeof[T]() + 1
        }

        def make() i32 {
            ret 7
        }
        """,
    )

    second, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-function.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 2", label="generic function body change second stats")
    assert_contains(second.stderr, "reused-modules: 0", label="generic function body change second stats")
    assert_contains(
        second.stderr,
        "reused-module-objects: 0",
        label="generic function body change second stats",
    )


def test_object_bundle_invalidates_imported_generic_struct_methods_when_owner_body_changes(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "bundle_import_generic_struct_method/dep.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "bundle_import_generic_struct_method/main.lo",
        """
        import dep

        def main() i32 {
            var box dep.Box[i32] = dep.Box[i32](value = 7)
            ret box.get()
        }
        """,
    )
    cache_dir = compiler.output_path("import-generic-struct-method-cache")

    first, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-struct-method.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()
    assert_contains(first.stderr, "compiled-modules: 2", label="generic struct method body change first stats")
    assert_contains(first.stderr, "reused-modules: 0", label="generic struct method body change first stats")
    assert_contains(
        first.stderr,
        "reused-module-objects: 0",
        label="generic struct method body change first stats",
    )

    compiler.write_source(
        "bundle_import_generic_struct_method/dep.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                var copy T = self.value
                ret copy
            }
        }
        """,
    )

    second, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-struct-method.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 2", label="generic struct method body change second stats")
    assert_contains(second.stderr, "reused-modules: 0", label="generic struct method body change second stats")
    assert_contains(
        second.stderr,
        "reused-module-objects: 0",
        label="generic struct method body change second stats",
    )


def test_object_bundle_invalidates_imported_generic_instances_when_owner_visible_imports_change(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "bundle_import_generic_owner_context/helper.lo",
        """
        struct Box[T] {
            value T
        }
        """,
    )
    compiler.write_source(
        "bundle_import_generic_owner_context/dep.lo",
        """
        import helper

        def make_helper_ptr() helper.Box[i32]* {
            ret null
        }

        def take_helper_ptr[T](value helper.Box[T]*) helper.Box[T]* {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "bundle_import_generic_owner_context/main.lo",
        """
        import dep

        def main() i32 {
            var out = dep.take_helper_ptr(dep.make_helper_ptr())
            ret 0
        }
        """,
    )
    cache_dir = compiler.output_path("import-generic-owner-context-cache")

    first, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-owner-context.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    first.expect_ok()
    assert_contains(first.stderr, "compiled-modules: 3", label="generic owner import change first stats")
    assert_contains(first.stderr, "reused-modules: 0", label="generic owner import change first stats")
    assert_contains(
        first.stderr,
        "reused-module-objects: 0",
        label="generic owner import change first stats",
    )

    second, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-owner-context.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    second.expect_ok()
    assert_contains(second.stderr, "compiled-modules: 0", label="generic owner import change reuse stats")
    assert_contains(second.stderr, "reused-modules: 3", label="generic owner import change reuse stats")
    assert_contains(
        second.stderr,
        "reused-module-objects: 3",
        label="generic owner import change reuse stats",
    )

    compiler.write_source(
        "bundle_import_generic_owner_context/helper.lo",
        """
        struct Box[T] {
            value T
        }

        struct Marker {
            value i32
        }
        """,
    )

    third, _ = compiler.emit_obj_bundle(
        main_path,
        output_name="import-generic-owner-context.manifest",
        cache_dir=cache_dir,
        target="x86_64-unknown-linux-gnu",
        stats=True,
    )
    third.expect_ok()
    assert_contains(third.stderr, "compiled-modules: 3", label="generic owner import change third stats")
    assert_contains(third.stderr, "reused-modules: 0", label="generic owner import change third stats")
    assert_contains(
        third.stderr,
        "reused-module-objects: 0",
        label="generic owner import change third stats",
    )


def test_object_bundle_rejects_lto_mode(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_lto_entry.lo",
        """
        ret 0
        """,
    )
    rejected, _ = compiler.emit_obj_bundle(
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


def test_bitcode_bundle_emits_only_module_bitcode(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "bundle_bitcode.lo",
        """
        ret 0
        """,
    )

    result, manifest_path = compiler.emit_bc_bundle(
        input_path,
        output_name="bundle-bc.manifest",
        target="x86_64-unknown-linux-gnu",
    )
    result.expect_ok()
    manifest = manifest_path.read_text(encoding="utf-8")
    expected_bundle_dir = compiler.repo_root / "lona_cache" / f"{manifest_path.name}.d"
    assert_contains(manifest, "format\tlona-artifact-bundle-v1", label="bitcode bundle manifest")
    assert_contains(manifest, "kind\tbc", label="bitcode bundle manifest")
    assert_contains(manifest, "target\tx86_64-unknown-linux-gnu", label="bitcode bundle manifest")
    assert_contains(manifest, "artifact\tbc\t", label="bitcode bundle manifest")

    bitcode_paths = []
    for line in manifest.splitlines():
        parts = line.split("\t")
        if len(parts) == 4 and parts[0] == "artifact" and parts[1] == "bc":
            bitcode_paths.append(Path(parts[3]))

    assert bitcode_paths, "expected bitcode bundle to list at least one module"
    for bitcode_path in bitcode_paths:
        assert bitcode_path.is_file(), f"expected emitted bundle bitcode: {bitcode_path}"
        assert bitcode_path.stat().st_size > 0, f"expected non-empty bundle bitcode: {bitcode_path}"
        assert bitcode_path.suffix == ".bc"
        assert bitcode_path.parent == expected_bundle_dir
