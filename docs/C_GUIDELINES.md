# C Coding Guidelines

C in this project should read like a well-edited specification: every name says
what it is, every interface states who owns what, and the compiler and linters
catch violations before a human does.

These rules apply to all project-owned C, whether written by people or by AI
agents. Agents are the main audience: they imitate surrounding code, so every
rule here is either enforced by a tool (see Enforcement) or demonstrated in a
golden file. A rule that is neither will drift.

When a rule and readability conflict, readability wins, but the exception is
written down next to the code.

## Naming

Spell identifiers out. We are not saving characters; we are saving the reader's time.

- **Case:** `snake_case` for functions, variables, fields and types;
  `UPPER_SNAKE_CASE` for macros and enum constants.
- **Prefix:** every exported symbol starts with `sxml_`, then the module:
  `sxml_tree_append_child`.
- **Functions are verb phrases:** `sxml_reader_read_line`, not `sxml_rdln`.
  Predicates read as questions: `sxml_node_is_element`.
- **Types are noun phrases:** `sxml_parser`, `sxml_parser_configuration`.
  Use no `_t` suffix, which POSIX reserves.
- **Variables say what they hold, including units:** `byte_count`,
  `codepoint_count`, `timeout_milliseconds`, not `n`, `len`, `t`.
- **Loop indices** are named after what they index: `entry_index`, not `i`.
  A loop over a plain counter may use `index`.
- **Booleans** are named as true statements: `has_namespace`, `is_empty`.

**Glossary.** Each domain term has exactly one spelling, listed in
`docs/GLOSSARY.md`. New terms are added there before they appear in code.

**Banned abbreviations** (non-exhaustive, enforced by lint): `ctx`, `buf`,
`len`, `str`, `ptr`, `tmp`, `cnt`, `idx`, `num`, `cfg`, `res`, `ret`, `val`,
`msg`, `err` as a variable name.

Allowed short forms are only those the field itself uses as words: `utf8`,
`uri`, `id`, `io`, `xml`, `xdm`, `qname` (only for the XML QName concept).

## Function signatures

Every project-owned function and function-pointer type takes at most three
parameters, counting state and outputs. A required external callback signature
may exceed this, with the exception documented at the declaration.

Three parameters force each argument group to name a real concept. Common shapes:

- `owner, request, output` — act on an object with a described request and
  produce a result.
- `memory or allocator, configuration, output` — bring an object into existence
  (see Object lifecycle).
- `view, output` — a pure transformation of borrowed input.

Group arguments into structs that express a domain concept and a lifetime: a
byte view, a configuration, a request, a lookup key, a result. Never create a
generic `context` bag just to stay under the limit, and never move call-local
pointers into long-lived objects to shorten a signature.

Call grouped interfaces with designated initializers so every argument is named
at the call site:

```c
sxml_status status = sxml_parser_read_document(
    parser,
    &(sxml_read_request){
        .input = sxml_bytes_from_string(source_text),
        .preserve_whitespace = false,
    },
    &document);
```

Inputs come before outputs. Outputs are the last parameter and are pointers the
caller owns. A parameter that is intentionally both read and written is named
for that role (`cursor_state`) and documented as in/out.

Prefer two narrow functions over one function with a mode flag. A `bool` is
allowed only as a named struct field, never as a bare positional argument.

## Ownership and lifetimes

Every pointer in an interface is either borrowed or owned, and the declaration
says which.

- **Borrowed is the default.** A function that receives a pointer may use it only
  for the duration of the call and must not free or store it.
- **Storing a pointer** beyond the call is part of the contract and is
  documented: "`index` keeps a reference to `string_table`, which must outlive it."
- **Transfer of ownership** is visible in the name: `sxml_list_take_entry`,
  `sxml_buffer_release_bytes`. A function that returns owned memory says so and
  names how to release it.
- **Library code never calls `malloc` or `free` directly.** Memory comes from the
  caller or from an allocator interface.
- **Arenas for many small objects.** When objects are created in bulk and die
  together, allocate them from an arena and refer to them by index rather than
  pointer, so their lifetime is the arena's.

On failure, a function leaves its outputs either untouched or explicitly reset,
and the header states which.

## Object lifecycle and caller-provided memory

Every object type supports two lifecycles: one where the caller provides the
memory, and one where the library allocates it through an allocator. The
caller-provided path is the primitive; the allocating path is a thin convenience
built on it.

| Pair | Who owns the object's memory | What it does |
| --- | --- | --- |
| `initialize` / `finalize` | The caller | Places the object in caller memory; finalize releases internal resources but never the memory itself |
| `create` / `destroy` | The library, through the allocator | Allocates, then initializes; destroy finalizes, then frees through the same allocator |

The names are never mixed: `create` is always matched by `destroy`,
`initialize` by `finalize`.

**Memory requirements are queried, not guessed.** Each type has a requirements
function that reports size and alignment for a given configuration. The result
is deterministic, so callers may compute it once and cache it.

```c
typedef struct sxml_memory_requirements {
    size_t byte_count;
    size_t alignment;
} sxml_memory_requirements;

sxml_status sxml_parser_memory_requirements(
    const sxml_parser_configuration *configuration,
    sxml_memory_requirements *requirements);

sxml_status sxml_parser_initialize(
    sxml_memory_view memory,
    const sxml_parser_configuration *configuration,
    sxml_parser **parser);

void sxml_parser_finalize(sxml_parser *parser);

sxml_status sxml_parser_create(
    sxml_allocator allocator,
    const sxml_parser_configuration *configuration,
    sxml_parser **parser);

void sxml_parser_destroy(sxml_parser *parser);
```

Rules for both paths:

- **`initialize` never allocates on its own.** It checks that the memory is large
  enough and correctly aligned, and returns `SXML_STATUS_INSUFFICIENT_MEMORY` or
  `SXML_STATUS_MISALIGNED_MEMORY` otherwise.
- **Growth is explicit in the configuration.** An object that needs memory after
  initialization either receives an allocator in its configuration or works
  within a fixed capacity and reports `SXML_STATUS_CAPACITY_EXCEEDED`.
- **Objects do not move.** After `initialize`, the memory stays at the same
  address until `finalize`. A type that is safe to copy byte-for-byte says so.
- **Composite objects compose requirements**, rounded up for alignment, so a whole
  object graph can live in one caller-provided block.
- **`destroy` and `finalize` accept `NULL`** and do nothing.
- **`create` stores its allocator** inside the object so `destroy` needs no extra argument.
- **No fixed-size storage structs in public headers** unless the size is
  deliberately frozen as part of the ABI.

## Interfaces

An interface is a pair: a pointer to the implementation's state and a pointer to
a constant table of operations. The library depends on interfaces wherever the
host should be able to substitute behaviour: memory allocation, input and output
streams, document resolution, clocks.

```c
typedef struct sxml_allocation_request {
    size_t byte_count;
    size_t alignment;
} sxml_allocation_request;

typedef struct sxml_allocator_operations {
    sxml_status (*allocate)(void *state,
                            sxml_allocation_request request,
                            void **memory);
    void (*release)(void *state, void *memory);
} sxml_allocator_operations;

typedef struct sxml_allocator {
    void *state;
    const sxml_allocator_operations *operations;
} sxml_allocator;
```

- **Operations tables are `static const`** and shared by every instance.
  Per-instance data lives only in `state`.
- **Every operation takes `state` first** and follows the three-parameter limit.
- **Interfaces are passed by value.** They are two pointers.
- **Lifetime is stated where the interface is accepted.**
- **Calls go through small wrappers**, such as
  `sxml_allocator_allocate(allocator, request, &memory)`.
- **Every interface has a default implementation** and a test double.
- **Interfaces are not extended in place.** Publish a new interface type instead.

## Errors

Fallible functions return an `sxml_status` enum; results go through the output
parameter. Never signal errors through magic return values.

- **Status values are specific** and named for the condition:
  `SXML_STATUS_UNEXPECTED_END_OF_INPUT`.
- **Ignored errors fail the build.** Mark status-returning functions with the
  project's `SXML_NODISCARD` macro.
- **XPath, XQuery and XSLT dynamic errors** carry their specification error code
  (for example `FOAR0001`) in an error record that the caller can inspect.
- **Detailed diagnostics** go into an optional diagnostic record supplied by the
  caller, never into global state or `errno`.
- **Library code never calls `abort` or `exit`, and never prints.**
- **Every early return releases what the function acquired.**

## Memory and buffers

A pointer to data never travels without its length:

```c
typedef struct sxml_byte_view {
    const uint8_t *bytes;
    size_t byte_count;
} sxml_byte_view;

typedef struct sxml_memory_view {
    uint8_t *bytes;
    size_t byte_count;
} sxml_memory_view;
```

- **Units are in the name.** `byte_count` and `codepoint_count` are different things.
- **Strings are UTF-8 byte views, not NUL-terminated**, except at external boundaries.
- **Output buffers** state capacity and bytes written, and what happens when
  capacity is too small.
- **Aliasing is declared**; otherwise use `restrict`.
- **Untrusted input cannot cause unbounded work or memory.** Limits such as
  maximum nesting depth are part of the configuration.
- **No variable-length arrays and no `alloca`.**
- **Size arithmetic is checked** with checked-add and checked-multiply helpers.

## Modules and headers

- **One module, one header pair:** `include/sxml/tree.h` and `src/tree.c`.
- **Opaque types by default.** Expose fields only for plain value types such as
  views, configurations and requests.
- **Headers are self-contained** and use `#pragma once`.
- **Internal declarations** live in `src/*_internal.h`.
- **`static` for everything not exported**; only the public API is exported.
- **No global mutable state.**

## Types and expressions

Target C17, with C23 features behind portability macros. Code must build with
Clang, GCC, MSVC and the WASI SDK.

- **Integers:** `size_t` for sizes and counts in memory, fixed-width types for
  values with a defined range, never plain `int` or `long` for data.
- **Distinct index types** for distinct things (`sxml_node_index`,
  `sxml_string_index`) as single-field structs.
- **`bool`** for truth values.
- **Enums** for closed sets; `switch` on enums lists every case and has no `default`.
- **`const` everywhere it is true.**
- **One declaration per line**, at first use, initialized.
- **Macros only for what functions cannot do.**
- **No implicit conversions** that change sign or lose precision.

## Comments and documentation

Every public declaration has a contract comment covering what applies of:
purpose, ownership of each pointer, units, aliasing, behaviour on failure, and
thread safety.

- **Comments in implementations explain why**, not what.
- **Cite the specification** where behaviour follows it.
- **No commented-out code** and no `TODO` without an issue reference.

## Enforcement

Compiler, then linter, then structural search, then review. CI runs all of them.

**Compiler flags** (Clang and GCC): `-std=c17 -Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wvla -Wswitch-enum -Wimplicit-fallthrough`.

**clang-tidy:** see `.clang-tidy`.

**ast-grep rules** for banned abbreviations, `malloc`/`free` outside the
allocator module, pointer parameters without a length, and bare `bool` parameters.

**Runtime checks:** AddressSanitizer and UndefinedBehaviorSanitizer on every
test run; fuzzing of every parser entry point.

**Review rubric:** names spelled out and in the glossary; at most three
parameters with meaningful groups; ownership and units stated in the header;
failure behaviour defined; no exception without a written reason.

## Working with AI agents

1. **Golden files.** Every task names the golden module to match.
2. **Headers first.** Implementation tasks may not edit public headers.
3. **Glossary before code.**
4. **Small, gated tasks** with defined tests.
5. **Separate review pass** that reports findings without rewriting.
6. **Fix drift immediately.**
