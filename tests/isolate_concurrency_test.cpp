// ============================================================================
// saQut — Isolate eşzamanlılık testi (ADR-045 Faz 1, madde 1-h)
//
// Bir programı BİR KEZ derler (IRProgram + JIT CompiledProgram), sonra
// kThreads adet std::thread'de ayrı Isolate'lerle EŞZAMANLI koşturur:
//   - VM: her thread kendi Isolate'i + kendi Interpreter'ı (IRProgram salt
//     okunur paylaşılır); print çıktısı thread başına sink ile yakalanır.
//   - JIT: her thread kendi Isolate'i + IsolateGuard(iso, compiled) +
//     runOnIsolate (makine kodu ve ConstPool paylaşılır).
// Her thread'in dönüş değeri ve (VM'de) çıktısı, ana thread'de tek başına
// alınan referansla aynı olmalıdır.
//
// Program ConstPool string/decimal literallerine, struct/array tahsisine (GC)
// ve global yazımına dokunur. Global `g` her thread'de 7'den başlar ve bir
// artar: thread'ler globali paylaşsaydı sonuç değişirdi.
//
// Modlar: normal ve --gc-stress (eşik minimumda → sık toplama).
// Derleme: cmake --build build --target isolate_concurrency_test
// Koşu:    build/isolate_concurrency_test [--gc-stress]
// ============================================================================

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "diagnostic/diagnostic_engine.hpp"
#include "ir/ir_generator.hpp"
#include "mir/mir_backend.hpp"
#include "parser/parser.hpp"
#include "runtime/compiled_program.hpp"
#include "runtime/isolate.hpp"
#include "semantic/structural_validator.hpp"
#include "semantic/type_checker.hpp"
#include "symbol/symbol_collector.hpp"
#include "symbol/symbol_table.hpp"
#include "tokenizer/tokenizer.hpp"
#include "vm/interpreter.hpp"

namespace {

constexpr int kThreads = 4;

const char* kProgram = R"SQT(
struct Node {
    int v;
    string s;
}

int g = 7;
string gs = "abc";

int build(int n) {
    int sum = 0;
    int i = 0;
    while (i < n) {
        Node nd;
        nd.v = i;
        nd.s = gs + (i as string);
        int[] arr = [i, i + 1, i + 2];
        sum = sum + nd.v + arr[2] + nd.s.length();
        i = i + 1;
    }
    return sum;
}

int main() {
    g = g + 1;
    decimal d = 1.25;
    decimal e = d * 4;
    int acc = build(20000);
    string t = "xy" + "z";
    acc = acc + t.length() + (e as int) + g;
    print(acc);
    return acc % 200;
}
)SQT";

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  OK   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

struct VmResult {
    int         rc = -1;
    std::string out;
};

VmResult runVm(const IRProgram& program, bool gcStress) {
    Isolate      iso;
    IsolateGuard guard(iso);
    VmResult     r;
    Interpreter  vm(program);
    if (gcStress) vm.setGCThreshold(1);
    vm.setOutputSink([&r](const std::string& s) { r.out += s; });
    r.rc = vm.run();
    return r;
}

int runJit(const CompiledProgram& compiled) {
    Isolate      iso;
    IsolateGuard guard(iso, &compiled);
    int          rc = -1;
    mir_backend::runOnIsolate(compiled, iso, rc, {});
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    const bool gcStress = argc > 1 && std::strcmp(argv[1], "--gc-stress") == 0;
    std::printf("=== isolate_concurrency_test (%s) ===\n", gcStress ? "gc-stress" : "normal");

    // ── Tek sefer derleme (isolate'siz; ön-uç yalnız ana thread'de) ─────────
    Tokenizer        tokenizer;
    auto             tokens = tokenizer.scan(kProgram, "<isolate_test>");
    DiagnosticEngine diag;
    Parser           parser(&diag);
    ASTNode*         ast = parser.parse(tokens);
    SymbolTable      symbols;
    if (ast && !diag.hasErrors()) SymbolCollector(symbols, diag).collect(ast);
    if (ast && !diag.hasErrors()) {
        TypeChecker(symbols, diag).check(ast);
        StructuralValidator(diag).validate(ast);
    }
    if (!ast || diag.hasErrors()) {
        diag.printAll(std::cerr);
        return 1;
    }
    IRGenerator irGen;
    IRProgram   program = irGen.generate(ast, symbols, "<isolate_test>");
    FileRegistry::instance().freeze();

    // ── VM: referans + eşzamanlı ────────────────────────────────────────────
    const VmResult vmRef = runVm(program, gcStress);
    check(!vmRef.out.empty(), "VM referans çıktısı üretildi: " + vmRef.out);
    {
        std::vector<VmResult>    results(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] { results[t] = runVm(program, gcStress); });
        for (auto& th : threads) th.join();
        for (int t = 0; t < kThreads; ++t) {
            check(results[t].rc == vmRef.rc,
                  "VM thread " + std::to_string(t) + " dönüş " + std::to_string(results[t].rc));
            check(results[t].out == vmRef.out,
                  "VM thread " + std::to_string(t) + " çıktı " + results[t].out);
        }
    }

    // ── JIT: bir kez derle, referans + eşzamanlı ────────────────────────────
    if (gcStress) mir_backend::setGcThresholdForNextRun(1);
    mir_backend::UnsupportedReason   reason;
    std::unique_ptr<CompiledProgram> compiled = mir_backend::compileProgram(program, reason);
    check(compiled != nullptr, "JIT derlemesi (" + reason.functionName + " " + reason.opcodeName + ")");
    if (compiled) {
        const int jitRef = runJit(*compiled);
        check(jitRef == vmRef.rc, "JIT referans dönüş = VM referans (" + std::to_string(jitRef) + ")");
        std::vector<int>         results(kThreads, -1);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] { results[t] = runJit(*compiled); });
        for (auto& th : threads) th.join();
        for (int t = 0; t < kThreads; ++t)
            check(results[t] == jitRef,
                  "JIT thread " + std::to_string(t) + " dönüş " + std::to_string(results[t]));
        check(compiled->activeIsolates.load() == 0, "activeIsolates == 0 (yıkımdan önce)");
    }
    compiled.reset();  // tüm isolate'ler bitti → MIR_gen_finish/MIR_finish

    std::printf("%s (%d hata)\n", failures == 0 ? "TUM TESTLER GECTI" : "BASARISIZ", failures);
    return failures == 0 ? 0 : 1;
}
