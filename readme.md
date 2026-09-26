# saQut

> **A compiler built as a toolbox, not a black box —**
> every internal phase is a first-class, inspectable output.

```
saqut tokens  fib.sqt     →  token stream, JSON
saqut ast     fib.sqt     →  full AST, JSON
saqut ast     fib.sqt --optimized  →  constant-folded + DCE'd AST
saqut run     fib.sqt     →  execute via IR + bytecode VM
```

Most compilers are black boxes. saQut is a **glass box.**

---

## What is it?

saQut is a **procedural language compiler** written in C++.
The language is small and C-flavoured on purpose — it is a vehicle, not the product.
The product is **a compilation pipeline where every stage is named, queryable, and machine-readable.**

You can pipe `saqut ast` into your own tool.
You can hand the optimized AST diff to a review script.
A stranger with no access to source could write an LSP from `saqut symbols` output alone.
That is the test saQut is designed to pass.

---

## The language looks like this

```c
int fibonacci(int n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int fibonacciIterative(int n) {
    int first = 0;
    int second = 1;
    for (int i = 0; i < n; i = i + 1) {
        int next = first + second;
        first = second;
        second = next;
    }
    return first;
}

int main() {
    int n = 10;
    print(fibonacci(n));
    print(fibonacciIterative(n));
    return 0;
}
```

- No mandatory `class` / `main` boilerplate
- Typed functions, `struct`, `int[]` arrays
- `int`, `float`, `bool`, `string` literal types
- Value semantics — no user-visible pointers
- Single FFI seam (`callhost`) — the only door to the outside world

**Deliberately absent:** OOP, closures, generics, implicit int↔float coercion, `auto`.

---

## Build

**Requirements:** C++17, CMake ≥ 3.16, Ninja

```bash
git clone https://github.com/abdussamedulutas/saqut
cd saqut
cmake -B build -G Ninja
cmake --build build
```

Binary lands at `build/saqut`.

**Tested on:** Linux (x86-64, Manjaro). macOS and Windows untested but no platform-specific code.

---

## CLI

| Command | What you get |
|---|---|
| `saqut tokens src.sqt` | Token stream with positions |
| `saqut ast src.sqt` | Full AST as JSON |
| `saqut ast src.sqt --optimized` | AST after constant folding + dead-code elimination |
| `saqut symbols src.sqt` | Symbol table dump |
| `saqut check src.sqt` | Semantic analysis only — errors and warnings, JSON |
| `saqut ir src.sqt` | IR instruction dump |
| `saqut run src.sqt` | Compile and run via bytecode VM |

Every output is designed to be piped, diffed, or consumed by other tools.

---

## Pipeline

```
Source
  │  Lexer + Tokenizer
  ▼
Tokens ──────────────────── saqut tokens
  │  Pratt parser + recursive descent
  ▼
AST ─────────────────────── saqut ast
  │  Symbol collector (two-pass)
  ▼
Symbol Table ────────────── saqut symbols
  │  Type checker + structural validator
  ▼
Annotated AST
  │  Optimization Manager (clone — original untouched)
  │    ├─ Constant Folding pass
  │    └─ Dead Code Elimination pass
  ▼
Optimized AST ───────────── saqut ast --optimized
  │  IR Generator
  ▼
IR ──────────────────────── saqut ir
  │  Bytecode VM (interpreter loop)
  ▼
Output ──────────────────── saqut run
```

The optimizer works on a **clone** of the AST — the original is preserved.
Constant folding and DCE run in a fixpoint loop until nothing changes.

**Planned backends (ADR-032):** the VM stays as the reference backend; a
[MIR](https://github.com/vnmakarov/mir)-based JIT (`saqut run --jit`) and an
embedded-runtime AOT packager (`saqut build` — single self-contained executable,
no linker, no external toolchain) are next. WASM follows later; LLVM is
effectively out of scope for good.

---

## What works right now

| Stage | Status |
|---|---|
| Lexer / Tokenizer | ✅ |
| Pratt parser | ✅ |
| AST + JSON serialization | ✅ |
| Symbol table (two-pass collector) | ✅ |
| Type checker | ✅ |
| Structural validator | ✅ |
| Constant folding (int, bool, logical, unary) | ✅ |
| Dead code elimination | ✅ |
| IR generator + bytecode VM | ✅ |
| `saqut run` executes fibonacci | ✅ |
| `string` type | ✅ |
| `struct` | ✅ |
| `int[]` arrays | ✅ |
| Standard library / FFI beyond `print` | ✅ |

---

## Philosophy in two sentences

**Glass:** every compilation stage is a stable, queryable output — tokens, AST, symbols, IR — all separately inspectable and pipeable.
**Cage:** no user pointers, value semantics, single FFI door — the VM is deterministic, which makes record-replay and time-travel debugging a natural extension, not an afterthought.

The long version is in [`docs/architecture.md`](docs/architecture.md).

---

## Compatibility & determinism

The contract is **observed behavior** — stdout, return values, diagnostics, serialization output — never internal representation. Memory layout, value boxing, GC, and register allocation are the compiler's business and may differ between backends (the VM keeps strings inline, the JIT boxes them) and change between versions, as long as the observed result stays identical. Both backends (reference VM + MIR JIT) must produce the same output for the same IR; differential testing enforces this.

Versioning (binding from 1.0.0, SemVer):

- **Patch (`x.y.Z`)** — bug fixes only. A wrong result was never part of the contract, so it is corrected and backported to every affected minor line (`0.6.1` / `0.5.1` / `0.4.1`); the faulty `.0` is yanked.
- **Minor (`x.Y.0`)** — the observable surface is **frozen**: syntax, builtin/FFI signatures, whether a function exists, CLI output formats. Code is compatible **both ways** (a program written for 1.7 also runs on 1.3). Only the GC internals and performance change (same result, faster). **No new observable features in a minor** — a deliberate departure from SemVer's additive-minor norm, to protect the hand-written tooling ecosystem.
- **Major (`X.0.0`)** — a new product. Syntax, language identity, even this determinism rule itself may change. No compatibility promise across majors; intervals are long by design.

Serialization output (`toJson()` and friends) is frozen once shipped — improvements arrive under a new name (`toJson2()`) or a new major, never by silently changing the format.

---

## Design records

Architectural decisions live in `docs/`:

| File | Coverage |
|---|---|
| [`docs/fikirler.md`](docs/fikirler.md) | ADR-001–005: backend strategy, parser, header-only, token, IR |
| [`docs/adr-frontend-analiz.md`](docs/adr-frontend-analiz.md) | ADR-006–028: analysis, optimization, execution model, FFI, memory, semantics |
| [`docs/adr/`](docs/adr/) | ADR-029+: nested structs, heavyIR/lightIR, module cycles, MIR JIT + embedded-runtime AOT, JIT value ABI, determinism & versioning |
| [`docs/roadmap-frontend.md`](docs/roadmap-frontend.md) | Phase-by-phase implementation plan |
| [`docs/architecture.md`](docs/architecture.md) | Full architecture reference (Turkish) |

---

## License

Source-available, commercial use restricted.
Free for: personal use, learning, writing and running saQut programs, internal tooling.
Requires permission for: hosting as a service, embedding sub-components commercially, redistributing as a product.

See [`LICENSE.md`](LICENSE.md) for the full terms.
Commercial licensing: saqutsoftware+gitea@gmail.com
