# Runtime and Native Boundaries

> Consolidated knowledge-base document. Each numbered section preserves
> the responsibility and content of one former focused Markdown file.
> Former filenames remain recorded for traceability; internal links point
> to their new section locations.

## Section Map

- [22 Runtime](#kb-22-runtime)
- [23 Memory Model](#kb-23-memory)
- [24 Garbage Collection](#kb-24-gc)
- [25 Error Model](#kb-25-errors)
- [26 Diagnostics](#kb-26-diagnostics)
- [28 Serialization](#kb-28-serialization)
- [29 Curated FFI](#kb-29-ffi)
- [31 Builtins](#kb-31-builtins)
- [32 Standard Library](#kb-32-stdlib)
- [33 Security](#kb-33-security)
- [42 Concurrency](#kb-42-concurrency)

---

<a id="kb-22-runtime"></a>
## 22 Runtime

_Former file: `22_Runtime.md`._

### Status

**Implemented VM runtime services; backend-common thin runtime is only
Partially Implemented.** The self-hosted standard library remains Planned.

There is no single `src/runtime/` subsystem. Current runtime behavior is split
across `src/vm/`, builtin dispatch, curated FFI host functions, core decimal
support, and separate MIR trampolines.

### VM Runtime Boundary

The VM runtime begins after `IRProgram` generation. `Interpreter` provides:

- function frames, local/temporary slots, and flat global slots;
- dynamically tagged values;
- array/struct heap allocation and mark-sweep collection;
- arithmetic, casts, aggregates, control flow, and exception execution;
- builtin-method and curated FFI dispatch;
- capabilities, program arguments, output routing, and DAP controls.

Compiler parsing, symbols, type checking, and AST optimization are not runtime
services. Compile-time diagnostics are not language-level runtime `Error`
values.

### Values and Objects

VM `Value` represents scalar/string/decimal/date/null/reference data. Arrays and
structs begin at the heap/object boundary; strings and decimals remain inline
VM values. Detailed layout and lifetime rules live in [23_Memory.md](05_Runtime.md#kb-23-memory)
and [24_GC.md](05_Runtime.md#kb-24-gc).

### Builtins and FFI

Runtime host calls have three active shapes:

- special seeded builtin function calls such as `print`;
- builtin methods lowered as `CALLHOST("__builtin_method__", runtimeId)`;
- curated FFI lowered as `CALLHOST("__ffi__", hostFnId)` with optional
  capability metadata.

Builtin methods are compiler-known operations over language values. Curated FFI
is the explicit host/world boundary. They currently share `CALLHOST` transport
but have different registries, resolution, security, and extension rules.

Host C++ exceptions from builtin/FFI paths are converted by the VM to catchable
saQut `Error` values. Uncaught runtime errors escape the interpreter as C++
exceptions and are rendered by CLI callers.

### MIR Relationship

MIR JIT is not layered over the VM runtime. It has separate C trampolines for
its supported print, cast, string, decimal, and fatal-error paths. It rejects
general builtin methods, curated FFI, reference aggregates, try/throw, and other
unsupported IR at its whole-program gate.

The VM heap/GC and JIT object lifetime are different systems. Similar observable
intent does not establish shared implementation or verified parity.

### Self-Hosted Standard Library

ADR-041 is an accepted architectural direction: pure, language-expressible
operations should eventually live as embedded saQut `std/*.sqt`, leaving a thin
runtime of core intrinsics plus a curated host seam.

No implemented `std/*.sqt` source tree was found. Current string/array methods
remain VM C++ builtin implementations, and math/date helpers remain C++ FFI
even where ADR-041 proposes later migration. Treat the four-layer thin-runtime
diagram as **Accepted/Planned architecture**, not current implementation.

### Runtime Contract Rule

When adding an operation, classify it explicitly:

- core IR/backend primitive;
- builtin method;
- host/world FFI;
- planned self-hosted stdlib function.

Do not place behavior in multiple backends merely because the enum or registry
can represent it. Observable behavior requires tracked VM/JIT tests and was
**Not Verified** in this review.

See [20_VM.md](04_IR_Backends.md#kb-20-vm), [21_MIR.md](04_IR_Backends.md#kb-21-mir),
[29_FFI.md](05_Runtime.md#kb-29-ffi), and [32_Stdlib.md](05_Runtime.md#kb-32-stdlib).

---

<a id="kb-23-memory"></a>
## 23 Memory Model

_Former file: `23_Memory.md`._

### Status

**Implemented VM value/object representation; cross-backend representation and
ABI are Partially Implemented.** Lifetime behavior was not executed here.

### VM Value Representation

`src/vm/value.hpp` defines a tagged `Value` struct. It is not a compact union:
the struct contains fields for integers, floating data, decimal, string,
reference pointer, and int64 data, with `ValueKind` selecting the active
interpretation.

Active kinds are:

- `Int`; bool and byte also use integer storage;
- `LongInt`;
- `Float` and `Float32` (`Float32` is truncated to single precision);
- `Decimal`;
- `String`;
- `Date` stored as epoch-millisecond int64;
- `Null`;
- `Ref` containing `Object*`.

VM strings and decimals are inline value fields. Copying a `Value` copies those
values. Copying a `Ref` copies the pointer and creates aliasing.

### Slots and Call Lifetime

The VM is slot-based:

- every `CallFrame` owns a vector of parameter/local/temporary `Value` slots;
- `CALL` copies argument values into a new frame;
- `RETURN` copies a value into the caller destination;
- globals live in a single flat program-wide `Value` vector;
- popping a frame drops its slot roots but does not directly delete referenced
  heap objects.

The C++ `callStack_` vector is VM control storage, not an operand stack and not
the language heap.

### Heap Objects

`Heap` owns a non-moving intrusive list of `Object` instances. Active VM
allocation APIs create:

- `ArrayObject`, containing a vector of `Value` elements;
- `StructObject`, containing ordered `Value` fields and field names.

Arrays and structs are mutable reference objects. Equality in the VM is
identity-based for `Ref`; strings use content equality.

`ObjectType` also has `String` and `Decimal` categories for JIT boxing, but the
VM heap exposes no string/decimal allocation methods. An enum member does not
mean the VM stores those types on its GC heap.

### Ownership and Collection

The `Heap` owns all VM array/struct allocations until mark-sweep frees them or
the heap destructor releases the remainder. Slots/globals/pending errors are
non-owning roots. Aggregate elements and fields can recursively reference other
heap objects.

The language source has no user-visible manual `free`/ownership API in this
runtime model. Alias and mutation behavior therefore depends on reference
identity plus GC reachability.

### MIR JIT Boundary

MIR registers carry native scalar representations. Supported strings and
decimals are boxed as `StringObject`/`DecimalObject` pointers, but their
instances live in separate host-side vectors inside `mir_backend.cpp`. They are
cleared after the compiled program returns and are not linked into VM `Heap`.

Reference aggregates, shared VM/JIT object roots, and a JIT shadow stack are not
implemented in the active MIR support set. JIT allocation can grow for the
program duration.

Internal VM `Value` layout and MIR register/boxed layout are not a stable native
ABI. Observable language semantics may be common while representation differs.

### Change Rule

A memory-model change must audit `Type`, IR `SlotType`, VM `Value`, object
tracing, builtin/FFI argument handling, MIR register types/trampolines, equality,
serialization/debug views, and tracked backend differential tests.

See [16_Types.md](02_Language.md#kb-16-types), [20_VM.md](04_IR_Backends.md#kb-20-vm),
[24_GC.md](05_Runtime.md#kb-24-gc), and [30_ABI.md](04_IR_Backends.md#kb-30-abi).

---

<a id="kb-24-gc"></a>
## 24 Garbage Collection

_Former file: `24_GC.md`._

### Status

**Implemented VM non-moving mark-sweep collector; behavior and stress safety Not
Verified. MIR JIT GC integration is not implemented.**

### Heap and Algorithm

`src/vm/object.*` implements an intrusive linked list of VM heap objects. Each
object has a mark bit, type tag, next pointer, and virtual child-marking method.

Collection performs:

1. mark all reachable `Ref` values;
2. recursively mark array elements and struct fields;
3. sweep the intrusive list;
4. delete unmarked objects;
5. clear mark bits on survivors.

The collector is non-moving, so live `Object*` identities do not change.
Collection is stop-the-world with respect to the single interpreter loop; no
concurrent collector exists.

### Root Set

`Interpreter::maybeCollect()` marks:

- the flat global slot vector;
- every slot in every live call frame;
- `pendingThrow_`, when present.

`TryFrame` contains stack depth, catch instruction, and error-slot indices, not
object references, so it is not itself a value root. Aggregate children are
traced transitively.

Only `ValueKind::Ref` enters mark traversal. VM strings, decimals, numeric
values, dates, and null are inline/non-heap values.

### Safepoint and Trigger

`maybeCollect()` is called at the beginning of each VM instruction iteration,
before executing the next opcode. It runs only when automatic GC is enabled and
`heap_.allocCount` reaches the threshold.

The default initial threshold is 1024 live allocations. After collection the
next threshold becomes the larger of the initial threshold and twice the
surviving allocation count.

`--gc-threshold=N` changes this for `run`; a negative value disables automatic
collections while the `Heap` destructor still releases remaining objects.
`--gc-stats` reports collection counters after normal VM execution. These flags
are not wired identically into every CLI path.

### Object Coverage

VM allocation/tracing covers `ArrayObject` and `StructObject`, including Error
structs and FFI-created byte/string arrays. `ObjectType::String` and
`ObjectType::Decimal` exist for JIT boxing but are not allocated through VM
`Heap`.

Mark recursion uses the C++ call stack. Very deep/cyclic object graphs, threshold
behavior, root completeness, and allocation-during-host-call cases require
tracked stress tests and are **Not Verified** here.

### MIR JIT Separation

The MIR backend does not use this root set or collector. Its supported boxed
strings/decimals live in backend-global `unique_ptr` vectors until the compiled
program completes, then are cleared together.

There is no active JIT shadow stack, VM-heap registration, reference aggregate
support, or shared safepoint protocol. Do not describe the VM collector as
backend-common GC.

### Planned Work

JIT reference/aggregate support requires an explicit rooting ABI and collection
protocol. Possible shadow-stack comments in plans are **Planned**, not source
implementation. Any new VM root location must be added to `maybeCollect` before
the value can survive an instruction boundary.

See [23_Memory.md](05_Runtime.md#kb-23-memory), [20_VM.md](04_IR_Backends.md#kb-20-vm), and
[21_MIR.md](04_IR_Backends.md#kb-21-mir).

---

<a id="kb-25-errors"></a>
## 25 Error Model

_Former file: `25_Errors.md`._

### Status

**Implemented VM `Error` values and try/throw/catch IR; propagation edge cases
and backend parity are Partially Implemented / Not Verified.**

### Three Different Error Classes

Keep these mechanisms separate:

1. **Compiler diagnostics:** structured parser/module/symbol/semantic/optimizer
   reports accumulated in `DiagnosticEngine`.
2. **Language runtime errors:** saQut `Error` struct values propagated through
   VM `pendingThrow_` and catchable with `try`/`catch`.
3. **Host/compiler failures:** C++ exceptions, invalid invariants, I/O failures,
   or MIR fatal helpers rendered/terminated outside the language model.

A diagnostic is not a runtime `Error`, and a C++ exception is not automatically
part of the public language semantics.

### Builtin Error Value

`SymbolCollector::seedBuiltins()` defines a builtin struct named `Error` with a
layout shared with `Interpreter::makeErrorValue`:

```text
0 line: int
1 col: int
2 message: string
3 trace: string
4 code: string
```

This positional agreement is a cross-layer invariant between symbol layouts,
IR aggregate access, and VM construction.

### Throw and Catch

Parser source builds `TryStatementNode` and `ThrowStatementNode`. The collector
binds the catch variable as `Error` within a catch scope. TypeChecker analyzes
the thrown expression but deliberately allows any value.

IR uses:

- `ENTER_TRY` with catch instruction and error-slot metadata;
- `LEAVE_TRY` for normal completion;
- `THROW` with a source value slot.

VM `TryFrame` records call-stack depth, catch target, and catch error slot.
When a pending error exists, the VM unwinds frames to that depth, writes the
error into the catch slot, and jumps to the handler.

Throwing a non-struct value causes the VM to wrap its string representation in a
new `Error`. Throwing a struct is treated as an Error-like value and receives a
trace when enough fields exist. The frontend does not require the thrown struct
to be exactly `Error`; this permissive behavior is source-visible but its edge
semantics are **Not Verified**.

### Runtime Error Sources

The VM converts dynamic failures into pending `Error` values, including selected
division/modulo failures, bounds checks, casts, decimal overflow, builtin
failures, FFI failures, and missing capabilities.

Builtin/FFI C++ `std::runtime_error` exceptions are caught inside `CALLHOST` and
converted to `E_BUILTIN` or `E_FFI`. An uncaught saQut error escapes the VM as a
C++ `runtime_error` containing its message.

Early exits and nested try/call interactions rely on a separate `tryStack_`.
Their cleanup/unwind correctness requires tracked tests and is **Not Verified**.

### CLI and Exit Behavior

`run` and `exec` catch interpreter exceptions, print runtime-error text to
`stderr`, and normally return `1`. On successful execution, `run` returns the
program's `main` result as the process exit code. Compilation failures also
typically return `1`; no central typed exit-code policy exists.

MIR supports no try/catch IR. Some JIT trampoline failures print directly and
call `std::exit(1)`, so VM/JIT error parity is limited to the supported subset
and remains **Not Verified**.

See [20_VM.md](04_IR_Backends.md#kb-20-vm), [26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics), and
[27_Determinism.md](02_Language.md#kb-27-determinism).

---

<a id="kb-26-diagnostics"></a>
## 26 Diagnostics

_Former file: `26_Diagnostics.md`._

### Status

**Implemented structured diagnostic core; production, catalog, location, and
rendering consistency are Partially Implemented.**

### Data Model

`src/diagnostic/diagnostic.hpp` defines:

- `DiagLevel`: `Error`, `Warning`, `Note`, `Hint`;
- diagnostic code;
- one `SourceLocation`;
- message and optional hint;
- `tokenLength`, used to approximate an LSP range.

`DiagnosticEngine` stores reports in insertion order, exposes counts and
`hasErrors()`, and renders the same collection as terminal text, JSON, or LSP
diagnostics. It collects multiple errors; pipeline stages decide when to stop.

### Locations and Ranges

`SourceLocation` carries file, 1-based line/column, and 0-based offset.
`SourceFile` can convert two offsets into a start/end range, but `Diagnostic`
does not store that range. Most reports therefore have a single start point and
optional character length.

AST and IR source-location coverage is incomplete. Invalid/missing locations
render as `<invalid>`; do not fabricate spans.

### Production Points

Structured diagnostics are emitted by:

- parser when constructed with `DiagnosticEngine`;
- `ModuleLoader`;
- `SymbolCollector`, including imports/capabilities;
- `TypeChecker`;
- `StructuralValidator`;
- optimizer passes.

The tokenizer has no diagnostic-engine integration. Some single-file CLI paths
construct `Parser` without a diagnostic engine, causing direct `stderr`
messages. VM runtime errors use the separate language Error/exception path.

### Catalog and Severity

Updated #295 (2026-09-27): `src/diagnostic/diagnostic.hpp` holds the complete
registry — `diagnosticCatalog()` (every compile-time code: level, title,
explanation) and `runtimeErrorCatalog()` (every runtime `Error.code`).
`docs/compiler-errors.md` is generated from it (`gen_diagnostic_docs`; CTest
`diagnostic_docs_fresh` fails on a stale doc). CTest `diagnostic_codes`
checks that every code literal in `src/` is registered and every registered
code is produced. Call sites still pass string codes; the test, not the
compiler, catches a typo.

E003 now means only assignment/argument/return type mismatch; the former
E003 uses were split into E020 (literal range/context), E021 (operator
types), E022 (nullable without null check), E023 (invalid `as`), E024
(switch/case), E025 (not callable), E026 (method receiver), E027
(assignable location). ModuleLoader reports E_MODULE_NOT_FOUND/E_MODULE_PARSE
at the import's location.

LSP conversion maps errors to severity 1 and every non-error level to severity
2. Note and Hint are therefore not preserved as distinct LSP severities.

### Rendering

`printAll` emits:

```text
file:line:column: level [code]: message
    hint: ...
```

It does not currently render source lines, carets, multi-line spans, related
locations, fix-its, or color through a dedicated pretty formatter. JSON output
is structured, while individual CLI commands do not use it uniformly.

### Why It Is Partial

- lexical errors bypass the engine;
- parser injection differs by command;
- code catalog and active producers have drift;
- full spans/related locations are absent;
- location coverage is incomplete;
- terminal, JSON, and LSP severity/range presentation differ;
- commands have separate output and exit-code choices;
- no deduplication or stable cross-module sorting contract is defined.

The integrated `run` pipeline gates after loading, symbols, and semantics.
`check` always emits JSON; other inspection commands use their own formats.
Behavior and exact output stability are **Not Verified** because nothing was
executed.

See [11_Lexer.md](03_Frontend.md#kb-11-lexer), [12_Parser.md](03_Frontend.md#kb-12-parser),
[25_Errors.md](05_Runtime.md#kb-25-errors), and [35_LSP.md](06_Tooling.md#kb-35-lsp).

---

<a id="kb-28-serialization"></a>
## 28 Serialization

_Former file: `28_Serialization.md`._

### Status

**Partially Implemented.** The repository contains several JSON-producing
surfaces, but no unified serializer, deserializer, schema registry, or stable
binary persistence layer. Output behavior is **Not Verified** because no command
or test was run in this review.

### Existing JSON Surfaces

| Surface | Source-visible mechanism | Contract status |
|---|---|---|
| AST and AST analysis | Each AST node implements manual `toJson()` output through `JsonObject`; `saqut ast --json` wraps the tree and analysis counts | Implemented debug/tool output; no schema version |
| Types and locations | `Type::toJsonObj()` and `SourceLocation::toJsonObj()` use nlohmann JSON | Implemented internal/tool representation |
| Diagnostics | `Diagnostic` and `DiagnosticEngine` produce CLI JSON and a separate LSP diagnostic shape | Implemented protocol/tool output |
| Symbols | `saqut symbols --json` assembles symbols, references, type detail, locations, and diagnostics | Implemented tool output |
| Runtime values | builtin `struct.toJson()` recursively emits structs and arrays | Partially Implemented user-visible serialization |
| LSP/DAP | JSON-RPC and DAP messages use nlohmann JSON | Implemented protocol transport, not compiler-state persistence |

`src/json.hpp` is an active AST analysis/helper header despite its broad name.
It is not a general JSON library. AST JSON itself is emitted by node methods
under `src/parser/nodes/`.

No token JSON serializer was found; `tokens` prints text. Active IR has a
human-oriented colored `dump()`, not JSON or a reloadable format. No AST, IR,
bytecode, or runtime-object `fromJson`/deserialize path was found.

### Runtime `struct.toJson()`

`src/vm/interpreter.cpp` serializes struct fields in layout-vector order and
array elements in index order. It handles primitive values, nested structs,
arrays, null, decimal text, and epoch-millisecond date values.

This implementation is incomplete as a general JSON contract:

- string escaping explicitly covers quote, backslash, newline, and tab, but not
  every JSON control character;
- floating formatting uses stream defaults and has no cross-platform canonical
  specification;
- reference cycles have no visible cycle detection;
- there is no inverse parser or round-trip API;
- MIR JIT support for this builtin is not established by enum/registry presence.

ADR-038 treats published serialization output as observable API, but the
compiler is still `0.8.0` and no conformance execution occurred here.

### Schema and Stability

No `schemaVersion` field or independent AST/symbol/diagnostic schema version was
found. Current CLI JSON is therefore version-coupled and should not be described
as a stable public persistence format.

Keep these contract classes separate:

1. **Protocol JSON:** LSP/DAP shapes are governed by their protocols.
2. **Tool/debug JSON:** AST, symbols, types, locations, and diagnostics expose
   compiler internals and may require coordinated tool updates when changed.
3. **Language serialization:** `struct.toJson()` is program-observable behavior.
4. **Persistence/binary:** absent; see [44_Binary.md](04_IR_Backends.md#kb-44-binary).

### Determinism and Change Rules

- Preserve explicit source/layout order; never serialize an
  `unordered_map` directly when order is observable.
- A field rename, omission rule, numeric formatting change, or ordering change
  is a contract change even if the in-memory type is unchanged.
- Add a schema/version contract before claiming a tool output is independently
  versioned.
- A public serializer needs tracked escaping, nesting, ordering, numeric,
  invalid-value, and round-trip tests. Fixture presence alone is not a passing
  result.
- Do not infer a public binary format from nlohmann JSON's vendored binary
  codecs; no active Saqut call path uses them.

Evidence: `src/parser/ast_json.hpp`, `src/parser/nodes/`, `src/json.hpp`,
`src/core/{location,type}.hpp`, `src/diagnostic/`, `src/cli/commands/`,
`src/vm/interpreter.cpp`, and ADR-038/041. Static review: 2026-07-25.

See [13_AST.md](03_Frontend.md#kb-13-ast), [26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics),
[27_Determinism.md](02_Language.md#kb-27-determinism), and [45_Versioning.md](07_Engineering.md#kb-45-versioning).

---

<a id="kb-29-ffi"></a>
## 29 Curated FFI

_Former file: `29_FFI.md`._

### Status

**Implemented curated declaration/catalog/VM dispatch path; backend coverage and
catalog drift checks are Partially Implemented.** Host behavior was not run.

### What “Curated” Means

This FFI is not arbitrary native-library loading and does not expose a stable C
ABI to saQut programs. The compiler owns a closed host API:

```text
embedded ffi declaration
  -> FfiCatalog
  -> import-bound Symbol with hostFnId/capability
  -> CALLHOST("__ffi__", numeric id)
  -> VM hostFnTable
  -> C++ implementation
```

`Value`, `HostContext`, and numeric table indices are internal C++ runtime
mechanisms, not a public native ABI.

### Declaration Catalog

`src/ffi/root_sqt.hpp` embeds saQut `ffi` declarations. `FfiCatalog` tokenizes
and parses that text once through a Meyers singleton, then indexes declarations
by module and function name.

Current declaration groups are:

- `core`: compiler version;
- `math`: scalar math and PI/E functions;
- `caps`: capability query/drop;
- `fs`: file and byte operations;
- `sys`: randomness, environment, sleep, and program arguments;
- `date`: current time, epoch conversion, arithmetic, fields, parse/format.

`Capability::Net` and `--allow-net` exist, but no network host function is
declared or registered. Enum/flag presence is not FFI support.

The embedded-catalog parser discards its local diagnostics. Declaration/host ID
drift is detected only when an imported symbol cannot find a registered host ID.
The comment that `core::version` is automatically available was not matched by
automatic symbol seeding; active binding still goes through FFI imports.

### Import and Dispatch

Unquoted module imports are skipped by `ModuleLoader` and resolved by
`SymbolCollector::resolveFfiImport`. Only imported names become callable
symbols. Each symbol receives:

- canonical function `Type`;
- parameter names;
- numeric `hostFnId`;
- FFI module name;
- optional required capability.

`IRGenerator` emits `CALLHOST("__ffi__")` with that ID and capability.
The VM creates `HostContext` pointing to its capability set, program arguments,
and heap, then calls the function table.

The table stores arity, but `callHostFn` itself does not enforce it. Normal
argument validation relies on frontend function types.

### Capability Model

Capabilities default closed and can be granted with `--allow-fs`,
`--allow-net`, or `--allow-sys`.

Enforcement is two-stage:

- SymbolCollector emits a compile-time diagnostic for a missing required
  capability;
- VM `CALLHOST` checks the capability again as a runtime backstop.

`caps::drop` can remove a capability at runtime; no FFI operation can add one.
The current exposed functions use `fs` and `sys`; `net` has no implementation.

### Errors and Backends

Host implementations may throw `std::runtime_error`; VM converts it to a
catchable `Error` with code `E_FFI`. Invalid numeric IDs return null from
`callHostFn`, so drift prevention remains important.

MIR JIT supports only its special one-argument `print` host path and rejects
general `__ffi__` calls. Curated FFI is therefore VM-only in the current
backend set.

### Stdlib Boundary

ADR-041 says world-facing atoms should remain FFI while pure expressible
helpers migrate to self-hosted stdlib. That migration is **Planned**; current
math/date implementations are still C++ host functions and no `std/*.sqt`
implementation was found.

See [22_Runtime.md](05_Runtime.md#kb-22-runtime), [30_ABI.md](04_IR_Backends.md#kb-30-abi),
[32_Stdlib.md](05_Runtime.md#kb-32-stdlib), and [33_Security.md](05_Runtime.md#kb-33-security).

---

<a id="kb-31-builtins"></a>
## 31 Builtins

_Former file: `31_Builtins.md`._

### Status

**Implemented registry and VM handlers for the current 26 builtin methods;
runtime behavior Not Verified. Registry-to-runtime linkage is manually coupled.**

### Two Builtin Shapes

Do not merge these:

- `print` is seeded directly as a builtin function symbol and lowered to a named
  host call.
- Value methods are `DataMethod` records in `src/data/` and lowered through a
  numeric runtime ID.

The method registry is not the curated FFI catalog and is not proof of a
backend intrinsic.

### Registry Model (updated #290, 2026-09-26)

Each data module (`src/data/{array,string,struct}.cpp`) returns a table of
`DataMethod` records: name, category, parameter rules, return rule, `retKind`,
`HOST_*` flags, and the thunk (body) in the same record.
`dataAllMethods()` (`src/data/data_registry.cpp`) concatenates them; the index
is the runtime ID and is kept stable (new entries appended, ADR-044). The
current name list is the tables themselves; it is not repeated here.

Compile-time lookup has one path shared by TypeChecker and LSP:
`dataReceiverCategory(Type)` (E[] → Array, string → StringVal, struct →
StructVal), `dataFindMethod(category, name)`, `dataMethodAcceptsReceiver`
(a fixed `params[0]` restricts the receiver, e.g. `toString` → `byte[]`), and
`dataMethodNames` for diagnostic hints.

Pool/List/Thread methods are not `DataMethod`s (they lower to dedicated
opcodes); their signatures live in one table,
`src/semantic/thread_intrinsics.hpp`, read by TypeChecker and LSP.

Parameter and return rules can refer to fixed types, receiver element type, or
receiver array type.

### Call Syntax and Resolution

TypeChecker handles three surfaces:

- primary dot/UFCS form: `value.method(...)`;
- category namespaces: `array::`, `string::`, `struct::`;
- deprecated element-type/struct-name prefixes, reported with `W006`.

Receiver type selects the method category. A struct field with the same name
shadows a dot-call builtin and is reported as non-callable rather than silently
choosing the builtin.

### Compiler-to-VM Link

1. TypeChecker looks up the registry entry, validates arguments, resolves the
   return type, and writes `builtinId` on `ScopeCallNode`.
2. IRGenerator emits `CALLHOST` with `kBuiltinBase + runtimeId`.
3. VM and JIT call the record's thunk through the shared host table
   (`rt_host_call`); there is no separate hand-ordered switch.
4. LSP reads the same registry for completion/signature information.

Builtin runtime failures become catchable `E_BUILTIN` errors in the VM.

### Backend and Layer Boundaries

MIR's support gate accepts only the separate one-argument `print` host call.
Builtin-method `CALLHOST` instructions reject the whole JIT program.

Current array/string/struct methods are C++ VM behavior, not backend-common
intrinsics and not self-hosted stdlib. ADR-041 accepts a future migration of
pure methods into saQut `std/*.sqt`; that source layer is **Planned**, not
implemented.

FFI differs by being import-gated, backed by embedded declarations, associated
with host/world capabilities, and dispatched through `hostFnTable`.

### Change Checklist

A new method requires synchronized registry signature, TypeChecker rules,
IR/runtime ID handling, VM implementation, LSP presentation, error behavior,
MIR rejection/support decision, public docs, and tracked runtime tests. A
registry entry alone is not an implemented builtin.

See [16_Types.md](02_Language.md#kb-16-types), [19_IR.md](04_IR_Backends.md#kb-19-ir),
[29_FFI.md](05_Runtime.md#kb-29-ffi), and [32_Stdlib.md](05_Runtime.md#kb-32-stdlib).

---

<a id="kb-32-stdlib"></a>
## 32 Standard Library

_Former file: `32_Stdlib.md`._

### Status

**Self-hosted standard library: Planned.** No tracked `std/*.sqt` library source
or compiler integration for such a tree was found. Current builtins and curated
C++ host functions are implemented infrastructure, not evidence that the
planned stdlib exists.

### Current Runtime-Facing Library Surface

The repository currently provides:

- a compiler-seeded builtin function/method registry;
- VM implementations for builtin operations;
- an embedded `root.sqt` declaration catalog;
- C++ `HostFn` implementations for `core`, `math`, `caps`, `fs`, `sys`, and
  `date`;
- capability metadata and VM enforcement for effectful host calls.

This surface is described in [31_Builtins.md](05_Runtime.md#kb-31-builtins) and
[29_FFI.md](05_Runtime.md#kb-29-ffi). Registry membership does not prove complete
typechecker/IR/VM/MIR behavior; execution was not verified here.

### ADR-041 Target Architecture

ADR-041 is **Accepted - locked decision**, but remains largely a target. It
separates:

1. backend-private execution machinery;
2. a small closed intrinsic/opcode set that every backend implements;
3. one curated host FFI seam for external state, entropy, system time, and
   audited native libraries;
4. self-hosted saQut library code for pure, expressible behavior.

The intended packaging is embedded saQut source compiled through the normal
module, semantic, and IR pipeline. The intended migration is vertical: freeze a
current behavior with tests, move one function/module, remove the old C++
implementation, and retain observable output.

### Implemented vs Planned Boundary

| Area | Status | Boundary |
|---|---|---|
| builtin registry and VM cases | Implemented | Current compiler/runtime behavior |
| curated `root.sqt` FFI catalog | Implemented | Embedded declarations, not stdlib modules |
| C++ host function table | Implemented | Includes pure functions that ADR-041 intends to migrate |
| `std/*.sqt` modules | Planned | No tracked source tree found |
| embedded stdlib source/cache | Planned | `root.sqt` is an existing seam, not proof of stdlib packaging |
| backend-independent stdlib IR | Planned | No self-hosted library compilation path identified |
| stdlib migration/conformance | Planned / Not Verified | ADR checklist exists; migration was not observed |

For example, date arithmetic and many string functions currently live in
C++/VM even where ADR-041 classifies their future home as self-hosted code.
Documentation must describe current behavior and separately label the target.

### Security and Backend Rules

Future stdlib code must not bypass capabilities. An effectful wrapper must reach
the same `CALLHOST` check as user code. Pure stdlib behavior must compile to the
closed intrinsic set plus allowed host calls; adding a backend must not require
rewriting every library function.

Current MIR only supports a restricted callhost/builtin subset. Therefore a
function working in VM is not automatically available through MIR.

### Documentation Rule

Public docs may document current builtin/FFI APIs only after checking compiler
source and tracked tests. Do not publish the ADR-041 module catalog as shipped
stdlib. Code fences for future stdlib pages must be classified under
[57_DocsSync.md](08_PublicDocs.md#kb-57-docssync).

See [22_Runtime.md](05_Runtime.md#kb-22-runtime), [27_Determinism.md](02_Language.md#kb-27-determinism), and
[33_Security.md](05_Runtime.md#kb-33-security).

---

<a id="kb-33-security"></a>
## 33 Security

_Former file: `33_Security.md`._

### Status

**Capability-gated curated FFI is implemented for the VM pipeline; a general
sandbox, memory-safety proof, and untrusted-code isolation are not.** Do not
describe saQut as a "safe language" from current evidence.

### Current Trust Boundary

User code reaches external state through compiler-defined builtins and the
embedded FFI catalog. It cannot declare arbitrary native symbols or load a
shared library through the active FFI model. This narrows the host boundary,
but host functions are trusted C++ code operating in the compiler process.

`Capability` defines `fs`, `net`, and `sys`. CLI defaults grant none and accepts
`--allow-fs`, `--allow-net`, and `--allow-sys`. Embedded FFI declarations carry
optional `requires <cap>` metadata through `Symbol`, `Instruction`, and VM
dispatch.

### Enforcement

- `SymbolCollector` reports `E_CAP_MISSING` while resolving an imported FFI
  function when the command did not receive its required capability.
- IR preserves `requiredCap`; `saqut ir --capabilities` reports a static upper
  bound.
- VM `CALLHOST("__ffi__")` checks the capability again before dispatch.
- `caps::drop` can remove an active capability; there is no host function that
  adds it back. The runtime check is therefore required after compilation.

This A+B path is source-visible for CLI `run`/`check`/`ir` and selected legacy
commands. Runtime behavior is **Not Verified** here. LSP and DAP construct
`SymbolCollector` without granted capabilities; DAP also does not apply launch
capability arguments. Capability-requiring programs may therefore diagnose or
fail differently in those tools: **Partially Implemented integration**.

### Explicit Limits

- `--allow-fs` is all-or-nothing. No read/write split, path allowlist, root
  confinement, symlink policy, or per-module authority is implemented.
- `net` exists in the enum/CLI, but no active network host catalog was found.
  An empty capability category is not network sandbox implementation.
- `sys` permits APIs involving environment, arguments, sleep, randomness, and
  time. It is intentionally nondeterministic and broad.
- No process isolation, syscall sandbox, CPU deadline, memory quota, recursion
  limit, output quota, or denial-of-service policy was found.
- VM bounds/error paths and GC code are not a memory-safety proof. MIR emits
  native machine code and uses raw pointers/private C trampolines; its support
  gate is a compatibility boundary, not a security boundary.
- Host functions can throw and access VM state through `HostContext`; registry
  review is part of the trusted computing base.

### Public Website and Future Web Tools

Compiler capabilities do not secure `saqut.com`, Astro, nginx, Cloudflare, or a
future playground. Deployment headers and observed HTTP behavior belong in
[58_WebDelivery.md](09_WebPlatform.md#kb-58-webdelivery). Any service executing user programs
needs OS/container isolation, resource limits, filesystem/network policy,
artifact cleanup, and abuse controls in addition to language capabilities.

The nested `saqutwebside/` repository is documentation infrastructure, not a
trusted compiler runtime. Generated pages and agent endpoints must never become
authority for security semantics.

### Change Rule

Any new host operation must define its effect class, required capability,
compile-time check, VM backstop, error behavior, deterministic implications,
and tool/test coverage. Pure convenience functions belong in the planned
self-hosted stdlib unless ADR-041's decision tree requires trusted native code.

See [29_FFI.md](05_Runtime.md#kb-29-ffi), [32_Stdlib.md](05_Runtime.md#kb-32-stdlib), and
[34_CLI.md](06_Tooling.md#kb-34-cli).

---

<a id="kb-42-concurrency"></a>
## 42 Concurrency

_Former file: `42_Concurrency.md`._

### Status

**No Saqut language/runtime concurrency model is implemented.** No supported
thread, async/await, task, channel, actor, fiber, or scheduler syntax/runtime
was found. Thread safety of compiler services and runtime instances is **Not
Verified and must not be assumed**.

### Current Single-Thread Model

- The VM mutates call frames, global slots, try state, heap, GC state, and
  pending errors without synchronization.
- VM mark-sweep root collection assumes a stopped interpreter state.
- MIR JIT source explicitly states a single-thread assumption. Runtime-created
  strings and decimals live in process-global mutable vectors, and codegen also
  contains mutable static counters.
- DAP reports one synthetic thread, ID `1`, named `main`; this is protocol
  compatibility, not multi-threaded execution.
- LSP/DAP request loops and document/debug state have no repository-level
  parallel scheduling contract.
- Module loaders, symbol/diagnostic stores, profilers, and registries are not
  documented as safe for shared concurrent mutation.

`sys.sleep` calls `std::this_thread::sleep_for`, but it only blocks the current
host execution path. It does not expose a Saqut thread API. `sys.random` uses
mutable static random generators without a visible lock, which further prevents
assuming concurrent host-call safety.

Function-local or read-mostly singleton initialization in the builtin/FFI
catalogs does not establish whole-compiler thread safety.

### Non-Evidence

- `synchronized` and `volatile` appear in tokenizer keyword tables, but no
  supported parser/semantic/runtime concurrency feature follows.
- DAP `threadId` fields do not imply language threads.
- Vendored MIR documents operations on separate contexts and contains pthread
  support. Those upstream facilities are not an active Saqut concurrency
  contract.
- Running separate `saqut` OS processes is process isolation, not a language
  memory model.

### Planned Direction

ADR-041 marks threads out of scope and mentions a post-1.0 isolation direction
where each state has one mutator. Old issue-generation notes mention future
concurrency, but no accepted scheduler, actor/fiber semantics, cancellation,
channel ordering, memory model, data-race rule, or GC coordination design was
found.

Accordingly:

- **Planned direction:** isolation/determinism may constrain a future model.
- **Unknown:** exact primitive, scheduling, fairness, ordering, and memory model.
- **Absent:** current user-facing concurrency API and thread-safe runtime claim.

### Change Rule

Concurrency requires an ADR before implementation. It must define observable
scheduling/determinism, ownership and sharing, FFI blocking, capability
propagation, VM/JIT root handling, diagnostics, cancellation, and debugger
semantics. Do not add ad hoc locks and then describe the language as concurrent.

Evidence: VM/heap/MIR/FFI/LSP/DAP sources, tokenizer keyword tables, vendored MIR
boundary, and ADR-041. Static review: 2026-07-25.

See [22_Runtime.md](05_Runtime.md#kb-22-runtime), [23_Memory.md](05_Runtime.md#kb-23-memory),
[24_GC.md](05_Runtime.md#kb-24-gc), and [27_Determinism.md](02_Language.md#kb-27-determinism).
