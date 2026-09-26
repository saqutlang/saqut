# saQut Compiler — Full Project Overview

> Written for another AI agent to gain a complete architectural understanding.
> Last updated: 2026-07-31

---

## 1. What is saQut?

saQut is a **procedural language compiler toolbox** written in C++17.
The language is C-flavored on purpose — it is a vehicle, not the product.
The product is **a compilation pipeline where every internal phase is a first-class, inspectable, machine-readable output.**

```
saqut tokens  fib.sqt     →  token stream, JSON
saqut ast     fib.sqt     →  full AST, JSON
saqut ast     fib.sqt --optimized  →  const-folded + DCE'd AST
saqut run     fib.sqt     →  execute via IR + bytecode VM
```

Source language: **saQut** — a procedural, statically-typed language with value semantics.
No OOP, no closures, no generics, no implicit coercion.

---

## 2. Repository Structure

```
saqutcompiler/
├── src/                      # ~24,773 lines of C++ (headers + sources)
│   ├── main.cpp              # Entry point, command dispatch
│   ├── json.hpp              # Single-header JSON library (vendored)
│   ├── tools.hpp             # ANSI color codes, helpers
│   │
│   ├── core/                 # Foundation types (no deps on other modules)
│   │   ├── type.hpp          # Type system: Type, PrimitiveKind, TypeKind
│   │   ├── config.hpp        # CompilerConfig (optimization flags, etc.)
│   │   ├── decimal.hpp       # DecimalValue (coefficient + exponent)
│   │   ├── capability.hpp    # Capability model for FFI security
│   │   ├── location.hpp      # SourceLocation
│   │   ├── module_registry.hpp
│   │   ├── sourcefile.cpp/hpp
│   │   └── array_elem_kind.hpp  # NEW (#206): packed array type tags
│   │
│   ├── tokenizer/            # Lexical analysis
│   │   ├── tokenizer.cpp/hpp # Character-by-character tokenizer
│   │   └── token.hpp         # TokenType enum
│   │
│   ├── lexer/                # Extended lexer
│   │   └── lexer.cpp/hpp
│   │
│   ├── parser/               # Pratt parser + recursive descent
│   │   ├── parser.cpp/hpp    # Top-level parser driver
│   │   ├── parser_base.hpp
│   │   ├── ast_node.hpp      # ASTNode base, ASTKind enum
│   │   ├── ast.hpp           # AST utilities
│   │   ├── ast_json.hpp      # AST → JSON serialization
│   │   ├── token.hpp         # Parser token
│   │   └── nodes/            # Per-node-type headers
│   │       ├── program.cpp/hpp
│   │       ├── declarations.cpp/hpp    # FunctionDecl, VariableDecl
│   │       ├── statements.cpp/hpp      # If, While, Return, etc.
│   │       ├── expressions.cpp/hpp     # ArrayLiteral, Cast
│   │       ├── binary_expr.cpp/hpp
│   │       ├── identifier.cpp/hpp
│   │       ├── literal.cpp/hpp
│   │       └── error_node.cpp/hpp
│   │
│   ├── symbol/               # Symbol collection (2-pass)
│   │   ├── symbol_collector.cpp/hpp
│   │   ├── symbol.hpp        # Symbol (function, variable, type)
│   │   ├── symbol_table.hpp  # SymbolTable
│   │   └── scope.hpp         # Scope management
│   │
│   ├── semantic/             # Type checking + structural validation
│   │   ├── type_checker.cpp/hpp    # ~1355 lines, main type system
│   │   └── structural_validator.cpp/hpp
│   │
│   ├── ir/                   # Intermediate Representation (3-address code)
│   │   ├── ir_generator.cpp/hpp    # AST → IR (2081 lines)
│   │   ├── instruction.hpp         # Instruction struct, Opcode enum (~400 lines)
│   │   ├── ir_function.cpp/hpp     # IRFunction, dump() formatting
│   │   ├── ir_program.cpp/hpp      # IRProgram (all functions)
│   │   └── ir.hpp
│   │
│   ├── opt/                  # AST-level optimizer
│   │   ├── optimization_pass.hpp     # Base class
│   │   ├── optimization_manager.hpp  # Pass runner (fixpoint)
│   │   ├── constant_folding.hpp      # ConstantFoldingPass ✅
│   │   ├── dead_code_elim.hpp        # DeadCodeElimPass ✅
│   │   └── ast_clone.hpp             # Deep clone for --optimized
│   │
│   ├── vm/                   # Reference backend (bytecode interpreter)
│   │   ├── interpreter.cpp/hpp    # ~1923 lines, main execution loop
│   │   ├── object.cpp/hpp         # Heap, GC, ArrayObject, StructObject
│   │   ├── value.hpp              # Value (tagged union, 160 lines)
│   │   └── call_frame.hpp         # CallFrame
│   │
│   ├── mir/                  # MIR JIT backend (EXPERIMENTAL)
│   │   ├── mir_backend.cpp/hpp    # MIR codegen (rejects array ops)
│   │   └── vendor/                # Vendored MIR library (DO NOT TOUCH)
│   │
│   ├── builtin/              # Builtin method registry
│   │   └── builtin_methods.hpp  # Method registration (UFCS dispatch)
│   │
│   ├── ffi/                  # Host FFI seam
│   │   ├── host_functions.cpp/hpp   # C++ host implementations
│   │   ├── ffi_catalog.hpp          # FFI function registry
│   │   ├── date_calc.hpp            # Date utilities
│   │   └── root_sqt.hpp             # Root stdlib declarations
│   │
│   ├── cli/                  # CLI commands
│   │   ├── args.hpp               # CliArgs parsing
│   │   ├── cli.hpp
│   │   ├── exit_codes.hpp          # Exit code constants
│   │   └── commands/
│   │       ├── run.hpp, check.hpp, ir.hpp, ast.hpp, ...
│   │       ├── tokens.hpp, symbols.hpp, exec.hpp
│   │       ├── bench.hpp, lsp.hpp, dap.hpp
│   │
│   ├── lsp/                  # Language Server Protocol
│   │   ├── lsp_server.cpp/hpp, lsp_handler.cpp/hpp
│   │   ├── json_rpc.cpp/hpp, document_store.cpp/hpp
│   │   └── lsp_types.hpp, uri.hpp, position.hpp
│   │
│   ├── dap/                  # Debug Adapter Protocol
│   │   ├── dap_server.cpp/hpp, dap_handler.cpp/hpp
│   │   ├── dap_types.hpp, frame_reader.hpp
│   │
│   ├── module/               # Multi-file module system
│   │   ├── module_loader.cpp/hpp
│   │   └── module_graph.hpp
│   │
│   ├── diagnostic/           # Error/warning reporting
│   │   ├── diagnostic_engine.hpp
│   │   └── diagnostic.hpp
│   │
│   ├── profiling/            # Phase-level profiling
│   │   └── stage_timer.hpp
│   │
│   └── bench/                # Benchmark support
│       └── profile.hpp
│
├── tests/
│   ├── golden/               # Golden output tests (70 tests)
│   │   ├── array/, builtin/, byte/, caps/, struct/, ...
│   │   └── *.expected
│   ├── general/              # General tests
│   │   └── crypto/           # Crypto stress tests (#206)
│   ├── bench/                # Performance benchmarks
│   ├── stress/               # Stress tests (GC, memory, UB)
│   ├── dap/                  # DAP protocol tests
│   ├── lsp/                  # LSP protocol tests
│   ├── module/               # Module system tests
│   ├── semantic/             # Semantic analysis tests
│   └── run.sh                # Test runner
│
├── docs/
│   ├── adr/                  # Architecture Decision Records (ADR-008..042)
│   ├── architecture.md       # 4-layer architecture model
│   ├── v1.0-kapsam-bildirgesi.md  # v1 scope declaration
│   ├── v0.9-v1.0-yol-haritasi.md  # Roadmap
│   ├── v1.0-issue-disposition.md  # Issue disposition for v1
│   ├── optimization-catalog.md    # 62-item optimization list
│   ├── licm-detayli-aciklama.md   # LICM deep dive
│   ├── jit-struct-entegrasyon-plani.md  # JIT+STRUCT plan
│   └── fikirler.md           # Future ideas
│
├── tasks/                    # Structured task artifacts
│   └── SQ-090-*/             # 0.9.0 milestone tasks
│
├── editor/vscode/            # VS Code extension (syntax highlighting)
├── saqutwebside/             # Web platform (Astro-based)
├── knowledge-base/           # AI agent knowledge files (09 topics)
├── AGENTS.md                 # Agent governance (binding rules)
├── CLAUDE.md                 # Project structure (auto-generated)
└── CMakeLists.txt            # Build system (CMake + Ninja)
```

---

## 3. Pipeline (End-to-End)

```
Source (.sqt)
  │  Tokenizer (tokenizer.cpp)
  ▼
Token Stream ──────────────── saqut tokens
  │  Pratt Parser (parser.cpp)
  ▼
AST ───────────────────────── saqut ast
  │  Symbol Collector (2-pass, symbol_collector.cpp)
  ▼
Symbol Table ──────────────── saqut symbols
  │  Type Checker (type_checker.cpp, 1355 lines)
  ▼
Annotated AST
  │  Optimization Manager (fixpoint, 2 passes)
  │    ├─ ConstantFoldingPass   ✅
  │    └─ DeadCodeElimPass      ✅
  ▼
Optimized AST ─────────────── saqut ast --optimized
  │  IR Generator (ir_generator.cpp, 2081 lines)
  ▼
IR (3-address code) ───────── saqut ir
  │  Bytecode VM (interpreter.cpp, 1923 lines)
  ▼
Output ────────────────────── saqut run
```

---

## 4. Language Features

```
Types:    int, float, double, bool, byte, longint, decimal, date, string
          char, void, enum, struct, T? (nullable), T[] (array)
          
Value semantics: all types are value types
    - struct → heap reference (shallow copy on assignment)
    - array  → heap reference
    - string → inline in Value (immutable)

Control flow:
    if/else, while, for (via while), switch, break, continue
    try/catch/throw (error values)

Functions:
    - First-class functions? NO
    - UFCS method syntax: arr.push(5) → E::push(E[], E)
    - export/import for modules
    - FFI seam: callhost("name", args)

No:
    OOP, closures, generics, implicit coercion, auto, pointers
```

---

## 5. Compilation Phases — Deep Dive

### Frontend

**Tokenizer** — Character-by-character tokenizer producing TokenStream.
Handles: identifiers, number literals (dec, hex, bin, float), string literals,
operators, comments. Produces `Token` structs with source location.

**Parser** — **Pratt parser** for expressions + **recursive descent** for statements.
The Pratt parser handles operator precedence elegantly:
- Prefix parsing (literals, identifiers, unary operators)
- Infix parsing (binary operators, member access, index, calls)
- Each operator has a binding power (precedence level)
- Expression grammar: `literal | identifier | unary | binary | member | index | call | cast`
  (`?` bir ifade operatörü değildir; yalnızca nullable tip işaretidir — `int?`)

**AST** — Typed node hierarchy rooted in `ASTNode` (tagged union via `ASTKind` enum).
- ~20 AST kinds: Program, FunctionDecl, VariableDecl, Block, IfStatement,
  WhileStatement, ReturnStatement, Break, Continue, BinaryExpression,
  LiteralNode, IdentifierNode, CallExpression, MemberAccess,
  IndexExpression, ArrayLiteral, CastExpression, etc.
- Each node is allocated on the heap (raw `new`), owned by parent/function.
- `resolvedType` field on ExpressionNode (set by type checker).

**Symbol Collection** — 2-pass:
1. Pass 1: Collect all declarations (functions, globals, structs, enums)
2. Pass 2: Resolve all references, build scope tree

**Type Checking** — Walks the AST, assigns `resolvedType` to every expression.
- Type inference: literals get type from context (byte context → byte)
- UFCS method resolution (E::push, E::length, etc.)
- Capability checking for FFI calls
- No implicit conversions (ADR-010) — explicit `as` required
- Nullable type tracking and narrowing (ADR-021)

### Backend

**IR Generator** — AST → 3-address Instruction stream.
- Each function produces a sequence of Instructions with slot-based operands.
- Slot types tracked via `finalizeSlotTypes()` (fixpoint over instructions).
- Supports backpatching for jump targets.

**Instruction Set** — ~50 opcodes in 5 categories:
```
Arithmetic:     ADD, SUB, MUL, DIV, MOD, FADD, FSUB, FMUL, FDIV, ...
Bitwise:        BAND, BOR, BXOR, SHL, SHR, BNOT (and L variants)
Control:        JMP, JIF_FALSE, JIF_TRUE, CALL, RETURN, CALLHOST
Memory:         LOAD_CONST, LOAD_STRING, LOAD_SLOT, LOAD_GLOBAL, STORE_GLOBAL
Array:          ARRAY_NEW, ARRAY_GET, ARRAY_SET, ARRAY_LEN
Struct:         STRUCT_NEW, FIELD_GET, FIELD_SET
Error:          ENTER_TRY, LEAVE_TRY, THROW
Conversion:     INT_TO_FLOAT, FLOAT_TO_INT, CAST_*, etc.
```

**VM** — Bytecode interpreter (hand-written, switch-based dispatch).
- `Interpreter::runUntilEvent()` — main execution loop
- Stop-the-world mark-sweep GC
- GC safepoints at instruction boundaries
- Packed arrays (#206): ArrayObject stores typed buffers (byte[], int[], etc.)
- Struct field names shared via std::shared_ptr

**MIR JIT** — EXPERIMENTAL. Uses [MIR](https://github.com/vnmakarov/mir) library.
Currently only supports scalar arithmetic, control flow, and print.
Arrays, structs, and most builtins are rejected as "unsupported opcode".

---

## 6. Key Architectural Decisions (ADRs)

| ADR | Title | Status |
|-----|-------|--------|
| 008 | Short-circuit logical operators | Done |
| 029 | Nested struct allocation | Done |
| 030 | HeavyIR/LightIR separation | On hold |
| 031 | Module cycle policy | Done |
| 032 | MIR JIT + embedded runtime + AOT | Active |
| 033 | Builtin syntax (UFCS) | Done |
| 034 | FFI declaration model | Done |
| 035 | Capability model | Done |
| 036 | Date type | Done |
| 037 | JIT Value ABI | Design |
| 038 | Determinism / version compat | Draft |
| 039 | IR type enrichment | Active (#206 related) |
| 040 | Numeric type widths | Done |
| 041 | Self-hosted stdlib + thin runtime | Vision |
| 042 | v1 feedback MVP + versioning | Binding for v1 |

---

## 7. Version Strategy (ADR-042)

```
0.8.0 — Published historical baseline (immutable)
0.9.0 — Verifiable preview (current active)
          → VM correctness, CLI surface, basic GC
1.0.0 — Feedback MVP
          → 7 CLI/tooling surfaces, narrow host/FFI, proof programs
```

v1 has **NO** stable JIT, AOT, concurrency, sandbox, WASM, or generics.

---

## 8. Open Issues & Blockers

### Issue #80 — struct support (BLOCKED ⛔)
**Label:** `akış:bloklu`
**Status:** This is a cornerstone issue. Struct support is incomplete — nested struct field access, struct return types, struct array semantics all have known gaps. Multiple downstream features depend on #80 being resolved first.

### Issue #206 — byte[] super-linear slowdown (FIXED ✅)
Packed type-tagged arrays implemented. Now `byte[]` uses `vector<uint8_t>` instead of `vector<Value>`. 80× memory reduction, O(N²) → O(N) GC mark. DAP handler and all builtins updated.

### Issue #143 — AI agent workflow migration
Finalized the issue-centric workflow. AGENTS.md defines strict role separation (Chief Architect, Delivery Manager, Implementer, Adversarial Tester).

### Current optimization gaps:
- Only 2 AST passes exist (ConstantFolding, DCE)
- No IR-level optimization
- No LICM, no CSE, no inlining, no strength reduction
- Full catalog: `docs/optimization-catalog.md` (62 items)

---

## 9. Build & Test

```bash
# Build (Debug)
cmake -B build -G Ninja
cmake --build build -j

# Build (Release)
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build-rel -j

# Build (ASan)
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -G Ninja

# Run tests
ctest --test-dir build
bash tests/run.sh
```

**Current test count:** 187 ctest tests, all passing.
70 golden tests, 26 differential (VM≡JIT, 44 skipped), 5 stress tests.

---

## 10. Performance Profile

| Benchmark | Key Metric |
|-----------|-----------|
| Crypto 64KB (Release) | 2.08s |
| 50MB byte[] integrity | 100% correct, ~50MB RAM (old: ~4GB) |
| O(n²) vs O(n) (N=5000) | 17× difference |
| Loop invariant extraction | 2.5× improvement |
| GC byte storm (50K arrays) | 48 collections, 49K freed |

---

## 11. Key Classes & Their Line Counts

| Class/File | Lines | Role |
|-----------|-------|------|
| `interpreter.cpp` | 1923 | Main VM execution loop |
| `ir_generator.cpp` | 2081 | AST → IR translation |
| `type_checker.cpp` | 1355 | Type checking + inference |
| `type.hpp` | 349 | Type system definition |
| `instruction.hpp` | 401 | Instruction struct + Opcodes |
| `ir_function.cpp` | 313 | IR dump/serialization |
| `object.hpp` | 207 | Heap + GC + objects |
| `value.hpp` | 160 | Tagged value union |
| `parser.cpp` | ~600 | Pratt parser |
| `symbol_collector.cpp` | ~800 | Symbol table builder |
| `dap_handler.cpp` | 737 | Debug adapter protocol |
| `host_functions.cpp` | 361 | C++ FFI implementations |
| `builtin_methods.hpp` | 296 | Method registration |

---

## 12. Contact Points for Another AI

If you need to understand any component deeper, ask about:
1. **GC design** — Stop-the-world mark-sweep, safepoint at instruction boundaries
2. **Type system** — Type::prim, Type::elementType, rank-based numeric tower
3. **Value representation** — 160-byte tagged union, inline strings
4. **Instruction encoding** — Slot-based 3-address IR
5. **Optimizer framework** — pass registration, fixpoint loop
6. **Array packing** — New ArrayElemKind system (#206)
7. **Struct metadata** — Shared fieldNames via shared_ptr
8. **Capability model** — FFI security gating
9. **Module system** — Multi-file compilation with import/export
10. **Error handling** — try/catch/throw with error values
