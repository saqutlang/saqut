# Developer Tooling

> Consolidated knowledge-base document. Each numbered section preserves
> the responsibility and content of one former focused Markdown file.
> Former filenames remain recorded for traceability; internal links point
> to their new section locations.

## Section Map

- [34 Command-Line Interface](#kb-34-cli)
- [35 LSP](#kb-35-lsp)
- [36 DAP](#kb-36-dap)
- [37 Formatter](#kb-37-formatter)
- [43 Packages](#kb-43-packages)

---

<a id="kb-34-cli"></a>
## 34 Command-Line Interface

_Former file: `34_CLI.md`._

### Status

**Implemented dispatcher with ten code-backed commands, four explicit command
stubs, and stubbed stdin mode.** Command behavior was not executed.

### Command Matrix

| Command | Source status | Pipeline |
|---|---|---|
| `run` | Implemented integrated path | ModuleLoader through VM or MIR |
| `check` | Implemented | Module graph through semantics; JSON diagnostics |
| `ir` | Implemented | Module graph through optional AST optimization and IR |
| `tokens` | Implemented inspection | Single file, tokenizer only |
| `symbols` | Implemented inspection | Single-file parser and symbols |
| `ast` | Partially Implemented inspection | Single-file analysis; optional partial clone optimizer |
| `exec` | Implemented synthetic path | Wraps input in synthetic `main`; single AST; VM/MIR |
| `bench` | Partially Implemented specialized path | Custom module discovery and repeated VM pipeline |
| `lsp` | Code-backed server | Protocol/runtime behavior Not Verified |
| `dap` | Code-backed server | Module pipeline + VM debugging; behavior Not Verified |
| `compile` | Stub/TODO | Prints not implemented, returns 1 |
| `parse` | Stub/TODO | Prints not implemented, returns 1 |
| `transpile` | Stub/TODO | Prints not implemented, returns 1 |
| `interpret` | Hidden Stub/TODO | Prints not implemented, returns 1 |

Registration in `main.cpp` does not make a TODO lambda an implemented command.

### Pipeline Differences

`run`, `check`, and `ir` directly use the canonical `ModuleLoader` graph.
LSP uses the loader with document overlays; DAP uses it when launching a
program.

`tokens`, `symbols`, and `ast` read one source file. Their parsers do not all
receive `DiagnosticEngine`. `exec` creates a synthetic program and does not
load source imports.

`bench` explicitly bypasses `ModuleLoader`, discovers dependencies itself, uses
parsers without structured diagnostics, does not mirror capability/program-arg
setup, and can catch a runtime exception without failing its complete command.
It measures a specialized path and must not define compiler semantics.

### Command and Option Registration (#291, 2026-09-26)

Each command defines one `CliCommand` (name, usage, description, positional
min/max, accepted `CliOption` bits, handler) in its `src/cli/commands/*.hpp`;
`src/cli/command_list.hpp` holds the single command list. All options live in
one table, `cliOptions()` in `src/cli/args.hpp`. `parseArgs`, `printHelp`, and
`dispatch` derive from these two tables; no command name or option list is
repeated elsewhere.

`parseArgs` rejects with usage error 64: unknown options, removed options
(`--format`, `--allow*`, `--capabilities`, with a reason), options the command
does not accept, bad numeric values, and wrong positional counts. Value options
accept `--x=V` and `--x V`. `--optimized` (no-op where `--dont-optimize` is
valid) and `--stdio` (`lsp`/`dap`) are hidden compatibility no-ops. The legacy
`file:`/`output:`/`ast:` argument prefixes were removed. `--compact` is still
accepted by `symbols` but has no effect outside its `--jsonl` rejection.

### Input, Output, and Exit Codes

`-` (stdin) is a usage error; programs are read from a file path only.

Commands write primary data/program output to `stdout` and diagnostics/status
mostly to `stderr`, but formatting is command-specific. There is no centralized
output schema.

Help/version return 0. Most usage, compilation, and runtime failures return 1.
`run` returns the successful program's `main` result. Inspection commands have
individual policies; for example `ast` can return 0 despite analysis
diagnostics. No central exit-code enum or stable public matrix exists.

See [08_Pipeline.md](03_Frontend.md#kb-08-pipeline), [26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics),
and [38_Build.md](07_Engineering.md#kb-38-build).

---

<a id="kb-35-lsp"></a>
## 35 LSP

_Former file: `35_LSP.md`._

### Status

**Implemented source-level stdio LSP server with compiler-pipeline reuse.**
Protocol correctness and the 21 tracked JSONL scenarios are **Not Verified**
because no server or tests were run.

### Transport and Lifecycle

`LspServer` reads and writes `Content-Length` framed JSON-RPC 2.0 on
stdin/stdout. Dispatch supports `initialize`, `initialized`, `shutdown`, and
`exit`, returning standard invalid-request/invalid-params/method-not-found
errors for selected malformed requests. Unexpected request exceptions become
`-32603`; malformed notifications are generally ignored.

The initialize response advertises full document sync (`textDocumentSync: 1`),
not incremental edits. `didChange` uses the last full-text change. UTF-16 is the
default position encoding; the server selects UTF-8 when offered. The reported
LSP server version is hard-coded as `0.1.0`, while the compiler CMake project is
`0.8.0`; do not treat that field as release authority.

### Document and Compiler Model

`DocumentStore` owns open-buffer content, line-start indices, entry AST/tokens,
symbol table, diagnostics, and exact token-offset symbol indices. Each update
runs:

`ModuleLoader with open-buffer overlay -> SymbolCollector -> TypeChecker -> StructuralValidator`

Open imported documents override disk content; unopened imports fall back to
disk. Canonical paths and per-location file paths support cross-file
definitions, references, rename edits, and grouped diagnostics. If no module
can be loaded, the previous symbol table is retained as a limited "last good"
fallback. Parser recovery allows semantic processing outside an error node.

The LSP does not execute IR/VM and does not use MIR. Its semantic quality is
bounded by the same parser/symbol/type/diagnostic gaps as the compiler.

### Source-Implemented Features

- publish diagnostics on open/change and clear them on close;
- definition, hover, references, document symbols, and document highlights;
- scope/type-aware completion for symbols, struct fields, and builtin/UFCS
  methods;
- cross-file rename with builtin/keyword rejection;
- signature help for user and builtin calls;
- UTF-16/UTF-8 position conversion and multi-file URI mapping.

No formatting, code actions, workspace symbols, semantic tokens, inlay hints,
or incremental parsing handlers were found.

### Known Boundaries

- The store compiles from the changed entry document; it is not a persistent
  workspace-wide dependency engine.
- Capability-requiring FFI imports are analyzed with an empty allowed-cap set;
  there is no LSP configuration path for capabilities.
- Some location repair searches forward from a declaration start within a
  bounded window. It is implementation recovery, not a general source map.
- `JsonRpc` appends timestamped request/response data to
  `/tmp/saqut-lsp.log`. This is outside protocol stdout and is an operational
  privacy/concurrency concern.
- The tracked JSONL fixtures structurally cover initialization, overlays,
  recovery, encoding, scope/cross-file navigation, completion, rename,
  signature help, and robustness. Registration is not proof they pass.

See [14_Symbols.md](03_Frontend.md#kb-14-symbols), [17_Modules.md](03_Frontend.md#kb-17-modules),
[26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics), and [39_Testing.md](07_Engineering.md#kb-39-testing).

---

<a id="kb-36-dap"></a>
## 36 DAP

_Former file: `36_DAP.md`._

### Status

**Implemented source-level DAP adapter for the VM backend.** Runtime protocol,
debug accuracy, pause behavior, and the 10 tracked JSONL scenarios are
**Not Verified** in this review.

### Architecture and Lifecycle

The DAP server uses `Content-Length` framing but DAP request/response/event
objects, not JSON-RPC envelopes. `FrameReader` is shared between the main loop
and execution polling. The source includes Linux/POSIX `poll.h` and `unistd.h`;
cross-platform support is **Not Verified**.

Implemented request handlers are:

`initialize`, `launch`, `setBreakpoints`, `configurationDone`, `continue`,
`next`, `stepIn`, `stepOut`, `pause`, `threads`, `stackTrace`, `scopes`,
`variables`, `evaluate`, `terminate`, and `disconnect`.

The adapter advertises no function/conditional breakpoints, step-back,
set-variable, goto, or exception-info support.

### Compile and Execution Path

`launch` runs the integrated module pipeline:

`ModuleLoader -> SymbolCollector -> TypeChecker -> StructuralValidator -> typed slot IR -> Interpreter`

It does not run the AST optimizer and does not support MIR JIT debugging. Build
failure emits only a generic `Build failed` output event in the current handler.
DAP constructs symbol collection with no allowed capabilities and provides no
launch-to-VM capability/program-argument mapping, so effectful FFI debugging is
an integration gap.

Program stdout is redirected from `Interpreter` to DAP `output` events, keeping
protocol stdout framed. The adapter models one thread named `main`.

### Breakpoints and Stepping

IR instructions carry source file/line metadata. The VM builds a
`(canonical file, line) -> first instruction` index. Source breakpoints are
verified only for executable indexed lines and stored in the VM.

`stepIn` advances one instruction. `next` and `stepOut` use VM call depth and
source-line state. Continue runs in bounded instruction chunks; between chunks
the adapter polls input so `pause` can stop an otherwise nonterminating
program. The chunk bounds pause latency but is not a user resource limit.

### Inspection

Stack frames expose function name, source file, and line. A single `Locals`
scope enumerates named IR slots; globals are not exposed. Structs and arrays get
recursive `variablesReference` handles, invalidated whenever execution resumes.

`evaluate` is deliberately limited to:

`identifier ( "." field | "[" nonnegative-integer "]" )*`

It does not execute arbitrary expressions, calls, or arithmetic. Cyclic/nested
values use bounded summaries, while child expansion remains explicit.

### Test Evidence Boundary

Tracked scenarios cover initialization, launch/configuration, stack/locals,
aggregate variables, output isolation, stop-on-entry, verified/unverified
breakpoints, pausing an infinite loop, and line stepping. These fixtures are
registered by CMake when Python is found; they were not executed here.

See [19_IR.md](04_IR_Backends.md#kb-19-ir), [20_VM.md](04_IR_Backends.md#kb-20-vm),
[26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics), and [39_Testing.md](07_Engineering.md#kb-39-testing).

---

<a id="kb-37-formatter"></a>
## 37 Formatter

_Former file: `37_Formatter.md`._

### Status

**Absent / Planned.** No Saqut source formatter implementation, `fmt`/`format`
CLI command, formatter test suite, or LSP formatting capability was found.

The CLI's `--format <json|text>` option is not a source formatter. It is parsed
into `CliArgs::format` and shown in help, but no command reads that field.
Existing machine-output switches are command-specific `--json` and `--compact`.
Therefore `--format` itself is a registered but currently inert output option.

### Current Source Limitations

The tokenizer discards whitespace, `//` comments, and `/* ... */` comments.
`TokenType::COMMENT` is reserved but the source explicitly says no comment token
is produced. The AST consequently does not retain trivia or comment attachment.

An AST-only pretty-printer built on today's tree would be unable to preserve
comments or original blank-line decisions. The VS Code language configuration
and TextMate grammar describe editing/highlighting behavior; they are not
formatters. The LSP initialize response does not advertise document, range, or
on-type formatting.

### Required Design Decision

Before implementation, choose and record how formatting owns source fidelity:

- retain comments/trivia in tokens or introduce a concrete-syntax/trivia layer;
- define attachment rules for leading, trailing, and documentation comments;
- decide behavior for syntax-error/recovery nodes;
- define line endings, indentation, brace placement, spacing, and maximum-line
  policy without changing program meaning.

These are **Planned requirements**, not current language contracts.

### Future Correctness Contract

One formatter engine should serve CLI and LSP. Completion requires tracked tests
for:

1. idempotence: `format(format(source)) == format(source)`;
2. comment and literal preservation;
3. successful reparse of formatted valid input;
4. semantic/IR equivalence before and after formatting;
5. stable cursor/range edits for LSP;
6. deterministic output across repeated runs.

A parser branch or AST printer alone is insufficient. Formatter changes must
audit syntax evolution and public examples, but public docs do not define the
grammar.

Evidence: `src/cli/args.hpp`, `src/cli/cli.hpp`,
`src/tokenizer/tokenizer.cpp`, `src/parser/token.hpp`,
`src/lsp/lsp_handler.cpp`, and `editor/vscode/`. Static review: 2026-07-25.

See [10_Syntax.md](02_Language.md#kb-10-syntax), [11_Lexer.md](03_Frontend.md#kb-11-lexer),
[12_Parser.md](03_Frontend.md#kb-12-parser), and [35_LSP.md](06_Tooling.md#kb-35-lsp).

---

<a id="kb-43-packages"></a>
## 43 Packages

_Former file: `43_Packages.md`._

### Status

**Package manager absent.** The compiler has a source module loader and an
embedded FFI module catalog, but no Saqut package manifest, lockfile, registry,
dependency solver, download/cache layer, package CLI, or implemented yank
mechanism.

### Module System Is Not Package Management

The active module system resolves:

- quoted imports relative to the importing file, canonicalized by
  `ModuleLoader`;
- bare module-name imports against the embedded `FfiCatalog`;
- export/import symbols across the resulting `ModuleGraph`;
- duplicate and circular source dependencies within that graph.

This resolves files and curated host modules for one compilation. It does not
select, acquire, authenticate, version, or install third-party dependencies.
See [17_Modules.md](03_Frontend.md#kb-17-modules).

`package` is present in tokenizer keyword tables and `KW_PACKAGE` exists in the
token enum, but no active parser/semantic/package-resolution implementation was
found. Keyword recognition is not language support.

### Missing Package Surfaces

No repository source establishes:

- a `saqut.toml` or other compiler package manifest;
- a lockfile format or content hash;
- package identity, namespace, source layout, or root discovery;
- SemVer range solving or deterministic dependency selection;
- registry, URL/Git dependency, cache, offline, or vendoring behavior;
- signatures, checksums, trust roots, advisories, or install scripts;
- `add`, `remove`, `install`, `update`, `publish`, or `yank` commands.

ADR-034 says bare module imports could later scale to installed packages; the
current resolver handles embedded FFI modules only. ADR-038 requires future
yank/backport hygiene and references package infrastructure, but acceptance of
that policy does not implement the infrastructure. Old issue-generation scripts
suggest URL imports, `saqut.toml`, and a separate `saqut-pkg`; these are discovery
notes, not accepted design.

### Repository Package Files Are Separate

`editor/vscode/package.json` and its npm lockfile package the VS Code extension.
The nested `saqutwebside/` repository has its own Astro/npm dependency system.
Neither is the Saqut language package manager, manifest, or registry.

### Future Boundary

A future package layer should resolve a locked package graph into concrete
source/module roots, after which the existing compiler module pipeline can load
files. Parser import syntax must not become an ad hoc network client.

Before implementation, an ADR must define identity, source/registry trust,
resolution determinism, lockfile ownership, cache integrity, offline behavior,
capability/security boundaries, version compatibility, and yank semantics.
Package artifacts and any binary payloads also require explicit formats; none
exist today.

Evidence: tokenizer/token enum, `src/module/`, `src/ffi/`, CLI registration,
ADR-034/038, editor package files, and old issue scripts. Static review:
2026-07-25.

See [33_Security.md](05_Runtime.md#kb-33-security), [44_Binary.md](04_IR_Backends.md#kb-44-binary), and
[45_Versioning.md](07_Engineering.md#kb-45-versioning).
