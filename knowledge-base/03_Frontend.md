# Compiler Frontend

> Consolidated knowledge-base document. Each numbered section preserves
> the responsibility and content of one former focused Markdown file.
> Former filenames remain recorded for traceability; internal links point
> to their new section locations.

## Section Map

- [08 Compiler Pipeline](#kb-08-pipeline)
- [11 Lexer and Tokenizer](#kb-11-lexer)
- [12 Parser](#kb-12-parser)
- [13 Abstract Syntax Tree](#kb-13-ast)
- [14 Symbols and Name Resolution](#kb-14-symbols)
- [15 Semantic Analysis](#kb-15-semantics)
- [17 Modules](#kb-17-modules)
- [18 Optimizer](#kb-18-optimizer)

---

<a id="kb-08-pipeline"></a>
## 08 Compiler Pipeline

_Former file: `08_Pipeline.md`._

### Status

**Implemented in source; runtime behavior Not Verified in this review.**

This document describes the integrated path visible in
`src/cli/commands/run.hpp`. Other CLI commands reuse parts of the pipeline but
do not all have identical construction, diagnostic, or ownership behavior.

### Integrated Flow

```text
entry source
  -> ModuleLoader
  -> Tokenizer using Lexer
  -> Parser
  -> ModuleGraph {module ASTs, tokens, paths, imports}
  -> SymbolCollector
       pass 1a: register top-level names
       pass 1b: resolve signatures and layouts
       import validation / FFI import binding
       pass 2: scopes, body traversal, identifier binding
  -> TypeChecker
  -> StructuralValidator
  -> optional AST optimization
  -> IRGenerator
  -> typed slot IRProgram
  -> VM (default/reference) OR whole-program MIR JIT (`--jit`)
```

### Stage Contracts

- `Lexer` is a character cursor and numeric/source-position helper. It does not
  emit the parser's token stream.
- `Tokenizer` owns the lexical classification step and returns token pointers.
- `Parser` consumes those tokens and returns a polymorphic AST.
- `ModuleLoader` repeats tokenization/parsing for the entry and imported files.
  Its move-only `ModuleGraph` owns transferred ASTs and token collections.
- `SymbolCollector` must run before type checking. It establishes bindings,
  function signatures, aggregate layouts, imports, and lexical scopes in a
  shared `SymbolTable`.
- `TypeChecker` annotates expression nodes and checks type/control-flow rules.
  `StructuralValidator` independently checks contextual placement rules.
- `OptimizationManager` mutates an already analyzed AST. The active optimizer is
  AST-level, not IR-level.
- `IRGenerator` lowers the analyzed graph to one `IRProgram` consumed by both
  execution backends.

### Diagnostic Gates

The `run` command stops after module loading, symbol collection, and semantic
analysis when `DiagnosticEngine` contains errors. Optimization diagnostics are
reported before IR generation. Do not move lowering or execution before these
gates without defining recovery semantics.

Parser diagnostics are not uniform across all commands: `ModuleLoader` supplies
a `DiagnosticEngine`, while some older/single-file command paths construct a
parser without one and can write syntax errors directly to `stderr`.

### Backend Selection

- Without `--jit`, `run` constructs `Interpreter`; this is the default and
  reference backend.
- With `--jit`, the same `IRProgram` is submitted to the MIR backend.
- MIR performs a whole-program support check. One unsupported instruction or
  slot kind rejects the complete JIT request.
- The active `run` and `exec` paths report JIT rejection; they do not silently
  execute the program in the VM.

### Command Variants

- `check` loads a module graph and runs symbols plus both semantic passes.
- `ir` follows the graph pipeline through optional AST optimization and IR dump.
- `ast` is a single-file inspection path and has different parser construction
  and clone behavior.
- `exec` is a separate source-string/single-AST path. It must not be assumed to
  prove module-pipeline parity.
- `bench` has specialized setup and should not define normal compiler semantics.

Pipeline duplication is a maintenance risk: a new semantic pass, error gate, or
IR prerequisite must be audited in every command that assembles stages.

### Evidence Boundaries

This review used source inspection only. No build or test was run. Therefore:

- the ordering and data structures above are **Implemented**;
- successful end-to-end execution and parity are **Not Verified**;
- comments, ADR references, and configured tests do not by themselves prove
  behavior.

See [11_Lexer.md](03_Frontend.md#kb-11-lexer), [15_Semantics.md](03_Frontend.md#kb-15-semantics),
[19_IR.md](04_IR_Backends.md#kb-19-ir), [20_VM.md](04_IR_Backends.md#kb-20-vm), and [21_MIR.md](04_IR_Backends.md#kb-21-mir).

---

<a id="kb-11-lexer"></a>
## 11 Lexer and Tokenizer

_Former file: `11_Lexer.md`._

### Status

**Implemented core scanning; lexical diagnostics are Partially Implemented.**
Behavior was not executed in this review.

### Responsibility Boundary

`src/lexer/lexer.*` and `src/tokenizer/tokenizer.*` are separate layers:

- `Lexer` owns the source text, character offset, backtracking stack,
  whitespace movement, numeric scanning, and `SourceFile` position lookup.
- `Tokenizer` owns token boundaries and classification. It recognizes comments,
  strings, numbers, delimiters, operators, keywords, and identifiers.
- The parser consumes the token list produced by `Tokenizer`, not `Lexer`
  directly.

Calling this entire pair only “the lexer” hides an important extension point:
new punctuation or keywords normally require tokenizer and token-table changes;
new character/numeric scanning behavior can require lexer changes.

### Token Production

`Tokenizer::scan()` repeatedly calls its scanner until end of input and returns
a `TokenList` (`std::vector<Token*>`). The caller owns those pointers until
ownership is transferred to `ModuleGraph`.

Classification (updated #296, 2026-09-27):

- explicit longest-form branches (character switch in `scope()`) for
  multi-character operators;
- `KEYWORD_MAP` in `src/tokenizer/token_kind.hpp` — the single keyword list,
  used by the tokenizer for keyword/identifier selection and for the token's
  `TokenType`;
- each token carries `category` (`TokenCategory`: identifier, keyword, number,
  string, operator, delimiter — the `saqut tokens` label and LSP input) and
  `kind` (`TokenType`, filled in `scan()` from `KEYWORD_MAP`/`OPERATOR_MAP`).
  The parser reads `kind`; it no longer reclassifies. `ParserToken` and the
  precedence table live in `src/parser/parser_token.hpp`.

Whitespace and comments are skipped and are not parser tokens. The end-of-input
sentinel is recognized by `TokenCategory::End` (not by text) and deleted.

The presence of a value in `TokenType` or the keyword map is not proof that the
parser, semantic layer, IR, and backends support that language feature.

### Literals

The character scanner recognizes integer bases 2, 8, 10, and 16 and decimal
floating/exponent forms through `INumber`. Numeric tokens preserve raw text and
base/float metadata for later parser and semantic work.

String tokens retain the raw lexeme and a decoded context. The tokenizer
contains escape handling for newline, tab, carriage return, backspace, and
pass-through escaped characters. Comments support line and block forms in
source.

End-to-end support must be traced beyond tokenization. For example, a primitive
type or token enum entry does not establish that a literal syntax exists for it.

### Source Location Model

`SourceLocation` stores:

- file path;
- 1-based line and column;
- 0-based source offset.

Tokens carry integer `start`/`end` offsets plus a start `SourceLocation`. There
is no shared full-span object with two `SourceLocation` endpoints in the parser
contract. `INumber` has both start and end locations, but this richer shape is
not the general token/AST representation.

Location coverage in later AST/IR stages is incomplete; do not reconstruct
missing locations from assumptions.

### Error Handling and Limits

The tokenizer takes an optional `DiagnosticEngine*` (ModuleLoader, `symbols`,
`exec` and the FFI catalog pass one; `tokens` and `bench` do not). It reports
E906 (unknown escape) and E907 (unterminated string). Known gaps:

- an unknown character becomes an empty identifier-like token after forced
  progress, with no lexical diagnostic;
- unterminated block comments reach EOF without a diagnostic;
- malformed-number behavior is handled locally rather than through the shared
  diagnostics layer.

Accordingly, token production is **Implemented**, while robust lexical error
reporting is **Partially Implemented**. Exact malformed-input behavior is **Not
Verified** until tracked tests are run.

### Change Checklist

When adding lexical syntax, audit `lexer.*`, `tokenizer.*`,
`tokenizer/token_kind.hpp`, `parser/parser_token.hpp` precedence, parser
denotation, diagnostics, formatter/highlighting consumers,
and tracked tests. Keep raw text, decoded value, offsets, and parser token type
semantically aligned.

See [12_Parser.md](03_Frontend.md#kb-12-parser) and [26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics).

---

<a id="kb-12-parser"></a>
## 12 Parser

_Former file: `12_Parser.md`._

### Status

**Implemented hybrid parser; error recovery is Partially Implemented.**
Parsing behavior was not executed in this review.

### Input and Output

`Parser::parse(TokenList)` consumes tokenizer-owned token pointers and returns
an `ASTNode*`, normally a `ProgramNode`. The parser does not own the token
objects. The resulting AST stores non-owning token references in literal and
identifier nodes.

The facade is `src/parser/parser.hpp`, the class contract is in
`parser_base.hpp`, and the active implementation is the single
`src/parser/parser.cpp` translation unit.

### Parsing Technique

The parser is hybrid:

- top-level declarations and statements use recursive descent;
- expressions use a Pratt/top-down precedence loop;
- null denotation parses literals, identifiers, grouping, array literals,
  prefixes, and other expression starts;
- left denotation parses calls, indexing, member/scope calls, casts, postfix,
  and binary operators.

`parseExpression(precedence)` keeps consuming while the next token binds more
tightly than the current threshold. Precedence is carried by `ParserToken`.
An explicit right-associativity helper/table exists, but no active use of that
helper was found in the expression loop; exact associativity for assignment and
power operators is therefore **Not Verified** from comments alone.

### Declarations and Statements

Source contains parser paths for:

- imports, exports, FFI declarations, functions, variables, structs, and enums;
- blocks, expressions, `if`, `while`, `for`, `do-while`, `return`, `break`,
  `continue`, `try`/`catch`, `throw`, and `switch`.

This list describes AST construction paths, not complete language support. Each
construct still requires symbol, semantic, IR, VM, formatter/LSP, and possibly
MIR handling.

Expression construction includes literals, identifiers, binary/prefix forms,
postfix forms, calls, array literals/indexing, member access, casts, and scope
or dot-call forms. Prefix operators currently reuse `BinaryExpressionNode` with
a missing left operand rather than a separate unary-node class.

### Error Reporting and Recovery

The parser optionally accepts `DiagnosticEngine*`:

- when supplied, it reports structured E9xx diagnostics with source location;
- when absent, legacy paths can write syntax errors directly to `stderr`.

`synchronizeAndMakeError()` implements panic-style recovery: it advances at
least once, skips toward `;`, `}`, or a known statement start, and returns an
`ErrorNode`. `parseProgram()` also has a progress guard.

Recovery is not comprehensive. Several expected closing delimiters are consumed
only when present, and the explicit synchronization path is concentrated around
unexpected expression statements. Downstream visitors mostly reach unknown
node kinds through default branches rather than a uniform `ErrorNode` contract.
Multi-error behavior and AST validity after malformed input are **Not
Verified**.

### Parser Boundary Rules

- Syntax acceptance does not imply semantic validity.
- Placement rules such as nested declarations or loop-only control statements
  belong to `StructuralValidator`, even if the parser can construct them.
- Name resolution, overload/builtin selection, and member validity do not belong
  in the parser.
- Parser type annotations are primarily textual names; canonical `Type` objects
  are established later.
- Never infer active grammar solely from token enums or comments.

### Extension Checklist

A grammar change must update tokenization, precedence/denotation, AST ownership,
structured diagnostics and recovery, AST JSON/clone behavior, symbols,
semantics, IR lowering, tooling, and tracked parser tests. New parser callers
should provide `DiagnosticEngine` rather than expanding the legacy `stderr`
path.

See [11_Lexer.md](03_Frontend.md#kb-11-lexer), [13_AST.md](03_Frontend.md#kb-13-ast), and
[15_Semantics.md](03_Frontend.md#kb-15-semantics).

---

<a id="kb-13-ast"></a>
## 13 Abstract Syntax Tree

_Former file: `13_AST.md`._

### Status

**Implemented polymorphic AST; location and clone coverage are Partially
Implemented.** Ownership behavior was not runtime-verified.

### Node Model

`src/parser/ast_node.hpp` defines the polymorphic `ASTNode` base and `ASTKind`.
Concrete nodes live under `src/parser/nodes/`.

The main categories are:

- `ProgramNode` and top-level declaration nodes;
- statement nodes, including `VariableDeclNode`;
- expression nodes;
- aggregate/error/control-flow-specific nodes.

`ExpressionNode` carries semantic annotations `resolvedType` and `isConstant`.
`StatementNode` carries `isReachable`. Identifiers can hold a non-owning
`resolvedSymbol`; scope/builtin call nodes can carry a resolved builtin ID.

There is no independent AST type-node hierarchy. Declaration and cast type
syntax is commonly stored as type-name strings, while semantic expression types
use `Type`.

### Ownership and Lifetime

The AST uses raw pointers and manual deletion:

- `ASTNode` owns pointers placed in its protected `children` vector and deletes
  them in its destructor.
- Many concrete nodes also own typed pointer fields such as `condition`,
  `body`, `arguments`, or `initExpr` and delete them in custom destructors.
- The design assumes a node is not simultaneously owned by both the generic
  child vector and a typed owning field unless its destructor explicitly
  accounts for that.
- Parent pointers are non-owning.
- literal/identifier token pointers are non-owning.
- resolved symbol pointers are non-owning and depend on `SymbolTable` lifetime.

`ModuleGraph` destroys ASTs before their token collections, preserving AST token
references during AST destruction. A refactor must audit every constructor,
`addChild`, typed field, destructor, clone, optimizer replacement, and CLI
cleanup path; converting isolated pointers without a complete ownership map is
unsafe.

### Relationships to Compiler Stages

- Parser constructs nodes and parent/child relations.
- `SymbolCollector` writes identifier bindings and references.
- `TypeChecker` writes expression types and some semantic resolution fields.
- AST optimizer reads annotations and may replace or remove nodes after
  semantics.
- `IRGenerator` lowers the analyzed/optimized AST.
- JSON/AST dump and LSP features inspect the same node model.

Optimization must preserve annotations needed by IR. A new node kind is not
complete until every relevant visitor has an explicit or intentionally safe
default behavior.

### Clone Contract

`src/opt/ast_clone.hpp` provides `deepClone` for the AST inspection/optimization
path. It rebinds parent pointers for handled nodes and intentionally shares
token pointers and `IdentifierNode::resolvedSymbol`.

Coverage is **Partially Implemented**: the switch explicitly covers a limited
set of older node kinds. Its default branch returns the original pointer, which
is not a deep clone and can create aliasing/double-ownership hazards when a
newer or unsupported node is cloned. Do not use the function as proof that all
current AST kinds are clone-safe.

### Source Locations and Serialization

Nodes carry one `SourceLocation`, not a full source span. Source comments and
construction paths indicate incomplete location coverage. Preserve known
locations, but mark missing ones rather than inventing offsets.

`ast_json.hpp` and node `toJson`/logging methods expose AST views. Those views
are tooling formats, not the compiler's semantic authority or a stable binary
serialization promise.

### Change Checklist

For each new or changed `ASTKind`, audit parser construction, parent linkage,
ownership/destruction, symbol and semantic visitors, clone, optimizer passes,
IR lowering, JSON/logging, LSP/DAP consumers, and tracked tests.

See [12_Parser.md](03_Frontend.md#kb-12-parser), [14_Symbols.md](03_Frontend.md#kb-14-symbols),
[15_Semantics.md](03_Frontend.md#kb-15-semantics), and [19_IR.md](04_IR_Backends.md#kb-19-ir).

---

<a id="kb-14-symbols"></a>
## 14 Symbols and Name Resolution

_Former file: `14_Symbols.md`._

### Status

**Implemented multi-pass collector and lexical scopes; some aggregate and
builtin modeling is partial.** Resolution behavior was not executed here.

### Data Model

`SymbolTable` owns `Symbol` and `Scope` objects with `unique_ptr`. Scopes form a
parent chain. Each scope has:

- an unordered local-name table;
- an insertion-order list;
- non-owning symbol pointers.

`resolve(name)` searches the current scope and then parents. `define` rejects a
duplicate in the same scope, while a child scope can shadow an outer name.

`SymbolKind` contains variable, function, parameter, struct, field, enum, and
enum-value categories. A `Symbol` also carries canonical `Type`, module ID,
definition/reference locations, parameter names, builtin state, and optional
FFI host/capability metadata.

Imports are not represented by a distinct `SymbolKind`. Source imports validate
access to already registered module symbols; FFI imports create function
symbols from `FfiCatalog`.

### Collection Order

`collectModuleGraph` uses one shared symbol table for the graph and performs:

1. Seed builtins.
2. Pass 1a over every module: register struct/enum names, placeholder function
   symbols, and top-level variables.
3. Pass 1b over every module: resolve function signatures, struct/enum layouts,
   and top-level types.
4. Run the struct-cycle hook.
5. Validate source and FFI imports.
6. Pass 2 over every module: create scopes, register parameters/locals, resolve
   identifiers, and record references.

All modules complete pass 1a before any pass 1b work, enabling forward and
cross-module type-name discovery.

The collector calls this a three-pass model by grouping name/layout work into
the first phase; the implementation exposes pass 1a, pass 1b, and pass 2.

### Scope Rules

- Function parameters live in a function scope.
- Function-body blocks create child scopes.
- Ordinary blocks, `for` statements, and catch bindings create scopes in the
  collector.
- Same-scope duplicates produce diagnostics.
- Local initializer expressions are traversed before the local is defined,
  preventing self-resolution and making earlier declarations visible before
  later ones.
- Member-field resolution is primarily deferred to `TypeChecker` and aggregate
  layout tables rather than lexical symbol lookup.

These rules are source-visible but exact edge behavior is **Not Verified**.

### Modules and Imports

The table is program-global, with `moduleId` and collected import sets enforcing
module boundaries. This is not a separate symbol table per module. Consequently
flat global-name collisions and import binding changes have program-wide
effects and require tracked multi-module tests.

Source imports are checked against module identity and export information.
Unqualified FFI imports resolve through the embedded FFI catalog and can attach
host function IDs and required capabilities.

### Partial Areas

- `checkStructCycles()` is currently an intentional no-op because struct fields
  are modeled with reference semantics; a future by-value aggregate model would
  need a real cycle check.
- `print` and `Error` are seeded directly. A source comment identifies broader
  builtin seeding/centralization as unfinished.
- `structLayouts` and `enumLayouts` are active shared maps. The existence of
  `Field` and `EnumValue` kinds does not prove all fields/members are materialized
  as lexical symbols.

### Cross-Layer Contract

Type checking and IR generation depend on symbol types, layouts, module IDs,
host IDs, and resolved AST pointers. LSP reads the same structures. Name
resolution must not be independently reimplemented in those consumers.

See [13_AST.md](03_Frontend.md#kb-13-ast), [15_Semantics.md](03_Frontend.md#kb-15-semantics), and
[17_Modules.md](03_Frontend.md#kb-17-modules).

---

<a id="kb-15-semantics"></a>
## 15 Semantic Analysis

_Former file: `15_Semantics.md`._

### Status

**Implemented TypeChecker and StructuralValidator with known partial coverage.**
No semantic tests or executable programs were run in this review.

### Analysis Order

For each parsed module, semantic analysis runs only after the shared
`SymbolCollector` has completed the module graph:

```text
bound AST + SymbolTable
  -> TypeChecker
  -> StructuralValidator
  -> diagnostic gate
  -> optional AST optimizer
```

The two visitors are independent. They share the AST and diagnostics engine but
do not form a single combined pass.

### TypeChecker Responsibilities

`src/semantic/type_checker.*`:

- assigns `resolvedType` to expression nodes;
- resolves identifier/function/member/index expression types from symbols and
  aggregate layouts;
- checks variable initialization, assignment, arguments, returns, and operator
  compatibility;
- resolves/checks builtin methods and call shapes;
- checks array, struct, enum, switch, cast, nullable, and comparison rules;
- applies limited flow-sensitive null narrowing;
- analyzes whether non-void function paths return or throw.

The checker uses `Type::Error` to suppress cascaded diagnostics. It does not
establish runtime success: bounds, division, fallible conversions, thrown
errors, host failures, and dynamic capability backstops remain runtime concerns.

### StructuralValidator Responsibilities

`src/semantic/structural_validator.*` checks syntax that the parser can build
but that is invalid in its surrounding context:

- `break` must be inside a loop or `switch`;
- `continue` must be inside a loop;
- `return` must be inside a function;
- function, struct, enum, and import declarations must not be nested in a
  function.

It tracks loop, pure-loop, and function context while walking statements. It is
not a type checker and does not resolve names.

### Parser, Semantics, and Runtime Boundary

- Parser owns grammatical shape and limited syntax recovery.
- Symbol collection owns lexical names, module access, signatures, layouts, and
  identifier bindings.
- TypeChecker owns static type compatibility and type annotations.
- StructuralValidator owns contextual placement restrictions.
- IR/VM retain defensive behavior for errors that are dynamic or that may reach
  them through incomplete/error-recovery paths.

Do not “fix” a semantic gap in the parser merely because the parser sees the
syntax first. Placement and type rules should remain in their established
visitors unless an ADR changes the boundary.

### Flow Analysis

Nullable narrowing recognizes simple named-variable null comparisons and
propagates selected facts through conditional logic. It is not a general SSA or
data-flow framework.

Return analysis recognizes structured exits such as return/throw and selected
branch/loop shapes. Exact completeness for complex nested control flow is
**Not Verified**. The AST optimizer's reachability flag is not a replacement
for semantic control-flow proof.

### Diagnostic Production

Diagnostics can originate before, during, and after semantics:

- tokenizer/parser and module loader;
- symbol/import collection;
- TypeChecker;
- StructuralValidator;
- optimizer warnings;
- VM runtime errors.

The integrated `run` pipeline stops before optimization/IR when semantic errors
exist. Some older CLI paths differ in parser diagnostic construction; see
[08_Pipeline.md](03_Frontend.md#kb-08-pipeline).

### Known Partial Areas

- Flow narrowing and return analysis are syntax-directed, not a full CFG
  analysis.
- Parser recovery nodes do not have a uniform explicit contract in all semantic
  visitors.
- Type-name parsing is duplicated in more than one layer; adding a type requires
  auditing collector, checker, IR generator, and backends.
- Language tokens/classes not represented in these visitors are not
  implemented semantics.

See [14_Symbols.md](03_Frontend.md#kb-14-symbols), [16_Types.md](02_Language.md#kb-16-types),
[25_Errors.md](05_Runtime.md#kb-25-errors), and [26_Diagnostics.md](05_Runtime.md#kb-26-diagnostics).

---

<a id="kb-17-modules"></a>
## 17 Modules

_Former file: `17_Modules.md`._

### Status

**Implemented source-file graph loading and cycle detection; import binding is
Partially Implemented.** No multi-module program was executed in this review.

### ModuleLoader

`src/module/module_loader.*` starts from an entry path, converts it with
`std::filesystem::weakly_canonical`, and recursively loads source imports.
For each file it:

1. detects whether the canonical path is already in the active load chain;
2. skips paths already present in `seen_`;
3. reads from an optional `SourceOverlay`, otherwise from disk;
4. tokenizes and parses with a shared `DiagnosticEngine`;
5. interns the canonical path in `ModuleRegistry`;
6. adds a `ModuleUnit`;
7. follows top-level quoted source imports.

Imports are resolved relative to the importing file's directory and then
canonicalized. There is no package search path, manifest resolution, or package
manager integration in this loader.

Although comments use “BFS-like” terminology, `loadUnit` is recursive. Consumers
must rely only on the documented invariant that `units[0]` is the entry unit;
the remaining order is not an API contract.

### ModuleGraph Ownership

`ModuleGraph` is move-only and owns a flat vector of `ModuleUnit` values. Each
unit contains canonical file path, module ID, AST pointer, and token pointers.
Its destructor deletes the AST before deleting its tokens.

`ModuleRegistry` maps paths to integer IDs. ID `0` is reserved for
`__builtin__`; `-1` means invalid/unassigned. Symbol and IR metadata carry these
IDs, but module visibility is enforced separately.

### Source Imports and Exports

Quoted imports such as `import {x} from "other.sqt"` enter the file graph.
`ModuleLoader` only loads/parses the target. `SymbolCollector::validateImports`
later checks that:

- the requested name exists in the selected source module;
- the declaration is marked exported;
- the importing module records access to the name.

Import binding is **Partially Implemented**: the collector identifies a source
unit using a path-suffix comparison against the raw import text rather than
reusing the loader's exact canonical resolution result. Same-suffix paths can
therefore be ambiguous. This must be repaired or protected with tracked tests
before treating path identity as fully reliable.

The current symbol architecture also uses one program-global table plus module
IDs/import sets, not isolated per-module namespaces.

### Embedded Module Imports

Unquoted module names such as `math`, `fs`, or `date` are not source files and
are intentionally skipped by `ModuleLoader`. `SymbolCollector` resolves them
through `FfiCatalog`; see [29_FFI.md](05_Runtime.md#kb-29-ffi).

### Circular Dependencies

`loadChain_` records the active recursive chain. Re-entering a path already in
that chain emits `E_MODULE_CYCLE` with the cycle path. A diamond dependency is
deduplicated through `seen_` and is intended to be valid.

Cycle detection exists in source and is **Implemented**; exact diagnostic and
edge behavior are **Not Verified**.

### Alternate Pipelines

- `run`, `check`, and `ir` use `ModuleLoader` directly.
- LSP document analysis uses it with an editor-buffer overlay; DAP program
  loading also uses it.
- `ast`, `symbols`, and `exec` use single-AST paths and do not represent
  multi-file module behavior.
- `bench` builds its own module graph and explicitly bypasses `ModuleLoader`.
  It lacks the same overlay/cycle/FFI-import handling and must not define module
  semantics.

See [08_Pipeline.md](03_Frontend.md#kb-08-pipeline), [14_Symbols.md](03_Frontend.md#kb-14-symbols), and
[35_LSP.md](06_Tooling.md#kb-35-lsp).

---

<a id="kb-18-optimizer"></a>
## 18 Optimizer

_Former file: `18_Optimizer.md`._

### Status

**Implemented small AST optimizer with two passes; coverage and arithmetic
safety are Partially Implemented.** No optimized program was executed.

### Layer and Activation

Optimization is performed on analyzed ASTs after symbol/type analysis and before
active IR generation. No active IR optimization pass was found.

`CompilerConfig` enables both current passes by default, but CLI compilation
invokes them only when `--optimized` is requested. Registration order is:

1. `ConstantFoldingPass`
2. `DeadCodeElimPass`

`OptimizationManager` repeats the complete ordered pass list until no pass
reports a change or `maxFixpointRounds` is reached; the default limit is 10.

### Pass Interface and AST Handling

Each `OptimizationPass` receives an AST root and optional `SymbolTable*`, keeps
ownership unchanged, and returns whether it transformed anything.

There are two manager entry points:

- `runPassesInPlace` mutates the supplied AST and is used by `run` and `ir`;
- `optimize` first calls `deepClone` and is used by `ast` for before/after
  inspection.

Clone coverage is partial and its fallback returns the original node pointer.
Therefore the clone-based path is not safe for every current AST kind; see
[13_AST.md](03_Frontend.md#kb-13-ast).

### Constant Folding

The implemented pass folds:

- binary expressions whose operands are int32 integer literals;
- arithmetic, comparison, bitwise, shift, and logical operators in its explicit
  switch;
- unary `!`, `~`, unary `-`, and unary `+` for boolean/int32 literals.

It creates a direct-value `LiteralNode`, preserves the source location and
existing `resolvedType`, and marks it constant. Division/modulo by literal zero
is not folded and emits `W002`.

It does not fold float, double, decimal, longint, string, date, aggregate,
identifier-derived constants, or general constant expressions. The supplied
symbol table is unused.

Risk: folding arithmetic uses ordinary C++ `int` operators and shifts, while
the VM has explicit wrapping/masked helpers. Overflow and invalid shift parity
are therefore **Not Verified** and must not be assumed from the pass name.

### Dead-Code Elimination

Within a `Block`, the implemented pass marks and removes statements after the
first direct `return`, `break`, or `continue`, emitting `W003`. It recursively
visits common block/control-flow bodies.

It is not CFG-based and does not prove branch conditions or function liveness.
Despite the warning hint mentioning `throw`, the terminator switch does not
include `ThrowStatement`. It also does not remove unused variables/functions or
perform algebraic simplification.

### Semantic Information

Pass decisions are mostly syntax/literal driven. Constant folding copies
semantic type annotations into replacement literals, but neither current pass
uses the `SymbolTable` argument for optimization reasoning. Optimization must
still run after semantics because IR expects annotations and resolved bindings.

### Diagnostic and Safety Rules

`run` and `ir` collect optimizer warnings in a separate `DiagnosticEngine`;
`ast` uses its analysis diagnostic engine. Warnings are printed but do not block
IR generation.

Any new pass must define ownership/replacement behavior, preserved annotations,
side-effect rules, source locations, termination/fixpoint behavior, and
VM/MIR-observable equivalence. Configured or comment-described optimization is
not implementation evidence.

See [13_AST.md](03_Frontend.md#kb-13-ast), [19_IR.md](04_IR_Backends.md#kb-19-ir), and
[27_Determinism.md](02_Language.md#kb-27-determinism).
