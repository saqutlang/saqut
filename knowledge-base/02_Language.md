# Language Contract

> Consolidated knowledge-base document. Each numbered section preserves
> the responsibility and content of one former focused Markdown file.
> Former filenames remain recorded for traceability; internal links point
> to their new section locations.

## Section Map

- [09 Language](#kb-09-language)
- [10 Syntax](#kb-10-syntax)
- [16 Type System](#kb-16-types)
- [27 Determinism](#kb-27-determinism)

---

<a id="kb-09-language"></a>
## 09 Language

_Former file: `09_Language.md`._

### Status

**Implemented procedural core; aggregate, nullable, error, module, and host-call
features are present but not every reserved word is a language feature.**
Runtime behavior is **Not Verified** in this static review.

### Current Identity

saQut is a statically checked, C-flavoured procedural language compiled through
an inspectable AST and typed slot IR. The active implementation is organized
around typed functions, variables, structured control flow, modules, structs,
enums, arrays, and explicit host calls. It has no user-visible pointer
declaration/dereference syntax and no active class, inheritance, interface,
closure, or generic type model.

"Glass box", VM-first/reference execution, determinism, and a future
systems-language/tooling ecosystem are design directions. Inspectable token,
AST, symbol, diagnostic, and IR outputs have source implementations. Complete
output stability, backend equivalence, record/replay, time-travel debugging,
AOT, and systems-language completeness are goals, not established runtime facts.

### Implemented Source Surface

Active source paths exist for:

- typed functions, parameters, globals, locals, and nested scopes;
- `if`/`else`, `while`, `for`, `do`/`while`, `switch`, `break`, `continue`,
  and `return`;
- structs, enums, arrays, strings, nullability, and explicit `as` casts;
- `try`/`catch`, `throw`, runtime `Error` values, and structured diagnostics;
- file modules with named imports/exports and embedded named FFI modules;
- primitive types described in [16_Types.md](02_Language.md#kb-16-types), including the
  partially supported edges documented there;
- builtin method syntax and curated FFI calls.

These are source-backed implementation claims, not claims that every
combination passes. Tracked tests are stronger behavior evidence, but they were
not run during this review.

### Partial or Misleading Surfaces

- `auto` enters declaration parsing, but no complete inference type or contract
  was found: **Partially Implemented**.
- `char` exists as a type, but no dedicated character literal path was found:
  **Partially Implemented**.
- `longint` and `date` can be accepted through identifier-shaped type parsing;
  keyword-table membership is not the definition of type support.
- Nullable analysis is targeted flow narrowing, not general alias-sensitive
  proof.
- MIR JIT supports only a whole-program-gated subset. VM is the reference
  backend; see [21_MIR.md](04_IR_Backends.md#kb-21-mir).
- A self-hosted standard library is **Planned**. Current C++ builtins and
  curated host functions are not that stdlib.

### Reserved Does Not Mean Supported

The keyword/token tables contain future or borrowed vocabulary such as
`class`, `interface`, `extends`, `implements`, `new`, access modifiers,
`finally`, `throws`, `assert`, `package`, `typedef`, `sizeof`, `constexpr`,
`native`, and concurrency-related modifiers. No corresponding complete
parser/AST/semantic/IR/runtime path was found. Treat these as reserved,
prototype residue, or unsupported until the full pipeline and authoritative
tests prove otherwise.

### Language Change Rule

A language feature is not implemented merely because it appears in a token
enum, parser branch, ADR, wiki, or public page. Confirm the complete path:

`tokenization -> parsing/AST -> symbols/types/validation -> IR -> VM`

Then audit MIR support, diagnostics, LSP/DAP, formatting, and tracked tests.
Compiler semantics follow the authority order in [01_Sources.md](00_Orientation.md#kb-01-sources);
public docs are not authoritative.

See [10_Syntax.md](02_Language.md#kb-10-syntax), [27_Determinism.md](02_Language.md#kb-27-determinism), and
[32_Stdlib.md](05_Runtime.md#kb-32-stdlib).

---

<a id="kb-10-syntax"></a>
## 10 Syntax

_Former file: `10_Syntax.md`._

### Status

**Implemented parser surface with known partial and contradictory operator
edges.** This file describes active parser code, not the larger token inventory.
End-to-end acceptance and behavior are **Not Verified** here.

### Declarations

The active parser recognizes these principal forms:

```text
Type name [= expression] [, name [= expression] ...] ;
Type function(Type param, Type[] param, ...) { statements }
struct Name { declarations }
enum Name { A, B = integer, C }
import {name, ...} from "relative-file.sqt";
import {name, ...} from embeddedModule;
export declaration
ffi Return name(params) : HOST_ID from module [requires capability];
```

`ffi` is primarily the embedded host catalog contract, not a native-library
declaration syntax. Types may be primitive keywords or identifiers. Arrays use
`Type[]`; parser compatibility also accepts a postfix `name[]` form. `T?`
expresses nullability. Multiple variables can share one declaration.

There is no active grammar for class/interface declarations, inheritance,
generics, packages, access modifiers, `const`, `typedef`, or `sizeof`.

### Statements

Implemented statement nodes and parser paths cover:

- blocks and expression statements, including an empty `;`;
- `if (expr) statement [else statement]`;
- `while (expr) statement`;
- `for (init; condition; update) statement`;
- `do statement while (expr);`;
- `return [expr];`, `break;`, and `continue;`;
- `switch (expr) { case expr [, expr ...]: ... default: ... }`;
- `try { ... } catch (Type name) { ... }`;
- `throw expr;`.

The parser accepts a catch type token but does not preserve/check that written
type; symbol collection binds the catch variable as `Error`. `finally`,
`throws`, and `assert` are tokenized but have no active statement/declaration
implementation. Switch lowering is designed without fallthrough; do not infer
C-style fallthrough from the surface syntax.

### Expressions

Primary/prefix forms include numeric, string, boolean, and null literals;
identifiers; grouping; array literals; and prefix `+`, `-`, `!`, `~`, `++`,
and `--`. Postfix/infix parsing covers:

- calls, indexing, postfix `++`/`--`;
- member access and UFCS-style `receiver.method(args)`;
- legacy/type scope calls such as `string::upper(value)`;
- explicit casts: `expression as Type` and `expression as Type?`;
- arithmetic, comparison, equality, logical, bitwise, shift, assignment, and
  compound-assignment tokens.

The parser also accepts `->` as member access, but the language has no
user-visible pointer model. Treat it as **Partial/unsupported**, not evidence of
pointers.

### Precedence and Associativity

`TokenPrecedence()` drives a Pratt expression loop. From tightest to loosest:

1. member/call/index;
2. postfix;
3. prefix binding;
4. `**` and `^`;
5. `* / %`;
6. `+ -`;
7. shifts and `as`;
8. relational, equality, bitwise `&`, bitwise `|`, logical `&&`, logical `||`;
9. `?`, then `:`;
10. assignment/compound assignment;
11. comma.

There are important source contradictions:

- `RightAssociative()` marks exponent, assignment, and ternary tokens as
  right-associative, but the active parser never calls it. The Pratt RHS uses
  the same precedence, which statically yields left grouping at equal
  precedence.
- `^` is described as exponentiation in the token file but IR lowers it to
  bitwise XOR.
- `**` has precedence/token support, but no complete IR lowering was found.
- `?`/`:` have precedence entries, but there is no active ternary AST/lowering
  contract; `?` is actively used for nullable types.

Therefore chained assignment associativity, exponentiation, and ternary
expressions must not be documented as supported until corrected and covered by
tracked tests.

### Recovery and Contract

The parser is recursive descent for declarations/statements and Pratt for
expressions. It reports `E9xx` diagnostics, emits `ErrorNode`, and performs
panic-mode synchronization at semicolons, braces, EOF, or known statement
starts. Several productions tolerate missing delimiters rather than reporting
immediately, so "an AST was produced" does not alone prove valid syntax.

Public syntax documentation must be synchronized under
[57_DocsSync.md](08_PublicDocs.md#kb-57-docssync). When docs disagree with source/tests, follow
[01_Sources.md](00_Orientation.md#kb-01-sources).

---

<a id="kb-16-types"></a>
## 16 Type System

_Former file: `16_Types.md`._

### Status

**Implemented core representation and major checking rules; several declared
types/syntaxes have partial end-to-end support.** Runtime behavior is Not
Verified in this review.

### Canonical Representation

`src/core/type.hpp` defines:

- `TypeKind`: `Primitive`, `Array`, `Struct`, `Enum`, `Function`, `Error`;
- primitives: `int`, `longint`, `float`, `double`, `decimal`, `byte`, `char`,
  `string`, `bool`, `void`, and `date`;
- recursive array element and function return types through `shared_ptr`;
- function parameter types;
- named struct/enum types;
- a `nullable` flag on any represented type.

`Type::equals` is structural and includes nullability. `Type::Error` is an
analysis sentinel used to prevent cascaded errors, not a runtime value type.

Parser declarations commonly retain textual type names. `SymbolCollector` and
`TypeChecker` convert those names to `Type`; IR then maps semantic/source types
to coarser `SlotType` categories.

### Builtin and User Types

Structs and enums have active parser, symbol-layout, semantic, IR, and VM source
paths. Arrays carry element type semantically and become reference values at
runtime. Function types are stored on function symbols as return and parameter
types.

No active class/interface type model was found. Token enum entries or syntax
comments are not evidence of implemented object-oriented types.

`auto` is recognized in parser declaration logic, but `Type::fromName` has no
`auto` type and no complete inference contract was identified. Treat it as
**Partially Implemented / Not Verified**, not established inference.

`char` exists in `Type`, but no dedicated character-literal tokenizer path was
identified. End-to-end `char` support is therefore **Partially Implemented**.

### Numeric Model

The source-visible model is:

- `int`: 32-bit runtime integer;
- `longint`: 64-bit signed integer, intentionally outside the normal
  int/float/double/decimal widening rank;
- `float`: 32-bit single-precision behavior;
- `double`: 64-bit floating behavior;
- `decimal`: `DecimalValue` fixed/explicit decimal representation;
- `byte`: range 0..255 in checked assignment/cast contexts and promoted to
  `int` for arithmetic.

`TypeChecker::numericRank` uses `int < float < double < decimal`. Non-literal
widening along this rank is accepted with a warning; narrowing requires an
explicit cast. Contextual numeric literals receive more permissive treatment.
`int -> longint` is allowed; other longint/numeric combinations generally
require explicit conversion.

The VM source defines two's-complement wrapping helpers for integer arithmetic
and masked shifts. That observable behavior is source-backed but **Not
Verified** by execution in this review.

### Nullability

`T` and `T?` are different types:

- the null literal is modeled as nullable `void`;
- null can be assigned only to a nullable target;
- `T` can flow to `T?`;
- `T?` cannot flow to `T` without proven narrowing or another explicit rule.

Narrowing is limited to recognizable named-variable null comparisons and
selected conditional combinations. It is not general alias-sensitive analysis.

### Operators and Assignment

Source checks include:

- numeric arithmetic and comparisons according to type rules;
- `byte` arithmetic promotion;
- `date` comparison without direct date arithmetic;
- string concatenation for `string + string`;
- string equality/inequality, while string ordering is rejected;
- nullable-operand restrictions;
- array indexing and aggregate member checks;
- `[index]` access on arrays and strings only; the index must be a non-null
  `int`, `byte` or `longint` (other types are `E003`, non-indexable receivers
  `E012`);
- `s[i]` on a non-null `string` yields the `i`-th character (0-based UTF-8 code
  point) as a one-character `string`, lowered to the same call as
  `s.charAt(i)` (same `E_HOST` out-of-bounds error); strings are immutable, so
  `s[i] = …` / `s[i] += …` are `E003`;
- function argument and return compatibility.

Runtime aggregate equality is identity-based for references, while strings use
content equality. Backend parity for these semantics remains **Not Verified**.

### Casts and Coercions

There is no general unconstrained implicit coercion. Assignment compatibility
is centralized in `TypeChecker::checkAssign`, with explicit exceptions for
literal context, widening, nullability, byte, and longint.

Explicit scalar casts cover multiple numeric/string/boolean/decimal paths.
Fallible casts can use nullable-result behavior or runtime throwing depending on
the target form. MIR supports only a subset and rejects nullable fallible-cast
targets; see [21_MIR.md](04_IR_Backends.md#kb-21-mir).

### Cross-Layer Audit Rule

Adding or changing a type requires auditing:

`Type` parsing/equality, symbol layouts/signatures, TypeChecker rules, AST
annotations, IR opcodes and slot inference, VM `Value`/operations, MIR support
gate/lowering/trampolines, builtin/FFI signatures, diagnostics, formatter/LSP,
and tracked VM/JIT tests.

See [15_Semantics.md](03_Frontend.md#kb-15-semantics), [19_IR.md](04_IR_Backends.md#kb-19-ir),
[20_VM.md](04_IR_Backends.md#kb-20-vm), and [21_MIR.md](04_IR_Backends.md#kb-21-mir).

---

<a id="kb-27-determinism"></a>
## 27 Determinism

_Former file: `27_Determinism.md`._

### Status

**Accepted design contract with partial implementation mechanisms.** Whole
program/backend equivalence, cross-platform reproducibility, and test results
are **Not Verified** in this review.

### Contract and Scope

ADR-038 defines determinism over observable behavior: stdout, return/exit
results, diagnostics, and serialized output. Heap layout, boxing, GC timing,
register allocation, and other internal representation may differ when they do
not change observations. VM is the reference backend; a MIR-accepted program is
intended to match VM behavior.

The ADR makes compatibility binding from `1.0.0`; the repository declares
`0.8.0`. Its freeze/backport/yank rules are therefore accepted direction, not a
current stable-ABI guarantee.

### Source-Visible Measures

- `IRProgram::functionOrder` and `globalNames` retain insertion order instead
  of exposing `unordered_map` iteration.
- diagnostics are accumulated in emission order; LSP groups diagnostic files
  in `std::map` order and explicitly sorts selected multi-location edits.
- ModuleLoader resolves imports in source order, canonicalizes paths, and
  detects cycles using an explicit load chain.
- VM integer operations use explicit wrap/shift helpers; `DecimalValue` uses a
  coefficient/exponent representation and normalized string output.
- tracked CMake differential tests compare VM and JIT stdout plus exit code for
  fixtures the JIT accepts.

These are **Implemented mechanisms**, not proof of complete determinism.

### Partial and Uncovered Areas

- Differential tests skip a fixture when the whole-program MIR gate rejects
  it. They do not prove parity for unsupported language features and do not
  compare all stderr, diagnostics, heap effects, or filesystem effects.
- `sys::random`, `sys::env`, `sys::args`, `sys::sleep`, `date::now`, and file
  operations intentionally observe external state. Capability checks control
  access; they do not make results deterministic.
- `float`/`double`, `std::pow`, host `libm`, formatting, NaN, denormals, and
  platform rounding are not governed by a complete cross-platform contract.
- `DecimalValue::fromDouble` and `toDouble` cross through binary floating point.
- canonical filesystem paths can vary by OS, filesystem, symlink state, and
  working tree location; no reproducible build-path policy was found.
- LSP writes timestamped logs to `/tmp/saqut-lsp.log`; this is an external debug
  artifact, not deterministic protocol output.
- stable serialization/binary formats, build reproducibility, package
  resolution, and deployed docs output remain separate incomplete contracts.

ADR-038 explicitly leaves endianness, cross-platform floating behavior, future
map iteration, time/random effects, and similar boundaries open.

### Developer Rule

Do not claim "deterministic" without naming the surface and environment. For a
behavior change:

1. define the observable output and allowed external inputs;
2. preserve explicit ordering instead of relying on hash iteration;
3. add VM golden coverage and VM/JIT differential coverage where MIR supports
   the feature;
4. classify unsupported MIR cases as missing coverage, never parity success;
5. record intentional nondeterminism at the FFI boundary;
6. treat benchmark timings as measurements, never deterministic output.

See [20_VM.md](04_IR_Backends.md#kb-20-vm), [21_MIR.md](04_IR_Backends.md#kb-21-mir),
[30_ABI.md](04_IR_Backends.md#kb-30-abi), and [39_Testing.md](07_Engineering.md#kb-39-testing).
