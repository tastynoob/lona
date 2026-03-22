# Indexable Pointer Plan

## Goal

Introduce `T[*]` as an indexable pointer type.

- `T*`: raw pointer, not indexable
- `T[*]`: indexable pointer, supports `p(i)` but still forbids pointer arithmetic
- `T[N]`: fixed-size array value

## Conversion Rules

- Allow implicit conversion only between `T*` and `T[*]`
- Require the same element type `T`
- Disallow conversions involving fixed-array pointers like `T[N]*`
- Disallow conversions involving function pointers

## Migration Rules

- `T[]` remains reserved for future slice/view work and is not implemented
- `T[]*` becomes invalid syntax/semantic usage with a targeted migration hint to `T[*]`

## Implementation Order

1. Add `IndexablePointerTypeNode` in the AST and parse `base_type '[' '*' ']'`
2. Add `IndexablePointerType` in the type system and lower it to an LLVM pointer to `T`
3. Replace the current unsized-array-view special cases in semantic validation
4. Make `T[*]` directly indexable via `p(i)` without `*p`
5. Restrict implicit pointer coercion to `T* <-> T[*]`
6. Update diagnostics, tests, and docs

## Tests

- Positive: `u8[*] = alloc(...)`, `p(0) = 1`
- Positive: nested indexable pointer forms like `i8[8]*[*]`
- Positive: mixed comparison `u8* == u8[*]`
- Negative: pointer arithmetic on both `T*` and `T[*]`
- Negative: `T* -> U[*]`, `T* -> T[N]*`, `T[N]* -> T[*]`
- Negative: old `T[]*` form with migration hint
