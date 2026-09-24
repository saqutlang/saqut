// ============================================================================
// saQut — ADR-045 Faz 1 assert'leri ölüm testleri (yalnız Debug build)
//
// Her senaryo fork edilmiş bir alt süreçte bir değişmezi BİLEREK ihlal eder
// ve alt sürecin SIGABRT ile (assert) sonlandığı doğrulanır:
//   1. Isolate::current() bağlı isolate yokken
//   2. FileRegistry::freeze() sonrası YENİ yol kaydı
//   3. runOnIsolate IsolateGuard önkoşulu (program bağlı değil)
//   4. ~CompiledProgram activeIsolates != 0 iken
// Ayrıca aynı senaryoların İHLALSİZ biçimi alt süreçte temiz çıkmalıdır
// (assert'in yanlış pozitif vermediği).
//
// embedProgramPtr üyelik assert'i derleyicinin iç (anonim) yardımcısıdır;
// dışarıdan ihlal edilemez — Debug build'de tüm JIT golden/diferansiyel
// koşuları onu her string/decimal literalinde değerlendirir (ihlalsiz).
//
// NDEBUG build'de assert'ler kapalıdır: test "atlandı" der ve 0 döner.
// ============================================================================

#include <csignal>
#include <cstdio>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "core/file_registry.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "ir/ir_generator.hpp"
#include "mir/mir_backend.hpp"
#include "parser/parser.hpp"
#include "runtime/compiled_program.hpp"
#include "runtime/isolate.hpp"
#include "semantic/type_checker.hpp"
#include "symbol/symbol_collector.hpp"
#include "symbol/symbol_table.hpp"
#include "tokenizer/tokenizer.hpp"

namespace {

int failures = 0;

// fn'i alt süreçte koşar; alt sürecin SIGABRT ile bitip bitmediğini döndürür.
bool abortsInChild(const std::function<void()>& fn) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);   // assert mesajı test çıktısını kirletmesin
        fn();
        std::_Exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void expect(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::unique_ptr<CompiledProgram> compileSmall(IRProgram& program) {
    Tokenizer        tokenizer;
    auto             tokens = tokenizer.scan("int main() { print(\"x\"); return 0; }", "<assert_test>");
    DiagnosticEngine diag;
    Parser           parser(&diag);
    ASTNode*         ast = parser.parse(tokens);
    SymbolTable      symbols;
    SymbolCollector(symbols, diag).collect(ast);
    TypeChecker(symbols, diag).check(ast);
    IRGenerator gen;
    program = gen.generate(ast, symbols, "<assert_test>");
    mir_backend::UnsupportedReason reason;
    return mir_backend::compileProgram(program, reason);
}

}  // namespace

int main() {
#ifdef NDEBUG
    std::printf("=== threading_asserts_test: atlandi (NDEBUG) ===\n");
    return 0;
#else
    std::printf("=== threading_asserts_test (Debug) ===\n");

    expect(abortsInChild([] { t_isolate = nullptr; (void)Isolate::current(); }),
           "Isolate::current() bagli isolate yokken assert");
    expect(!abortsInChild([] { Isolate::currentOrCreate(); (void)Isolate::current(); }),
           "Isolate::current() bagliyken temiz");

    expect(abortsInChild([] {
               FileRegistry::instance().intern("/assert/a.sqt");
               FileRegistry::instance().freeze();
               FileRegistry::instance().intern("/assert/b.sqt");
           }),
           "freeze sonrasi yeni yol kaydi assert");
    expect(!abortsInChild([] {
               FileRegistry::instance().intern("/assert/a.sqt");
               FileRegistry::instance().freeze();
               FileRegistry::instance().intern("/assert/a.sqt");   // kayitli yol: arama
           }),
           "freeze sonrasi kayitli yolun aranmasi temiz");

    expect(abortsInChild([] {
               IRProgram program;
               auto cp = compileSmall(program);
               Isolate iso;
               IsolateGuard guard(iso);   // program BAĞLANMADI → önkoşul ihlali
               int rc = 0;
               mir_backend::runOnIsolate(*cp, iso, rc, {});
           }),
           "runOnIsolate IsolateGuard onkosulu assert");
    expect(!abortsInChild([] {
               IRProgram program;
               auto cp = compileSmall(program);
               Isolate iso;
               IsolateGuard guard(iso, cp.get());
               int rc = 0;
               std::freopen("/dev/null", "w", stdout);
               mir_backend::runOnIsolate(*cp, iso, rc, {});
           }),
           "runOnIsolate dogru guard ile temiz");

    expect(abortsInChild([] {
               IRProgram program;
               auto cp = compileSmall(program);
               ++cp->activeIsolates;   // koşan isolate varken yıkım
               cp.reset();
           }),
           "~CompiledProgram activeIsolates != 0 assert");
    expect(!abortsInChild([] {
               IRProgram program;
               auto cp = compileSmall(program);
               cp.reset();
           }),
           "~CompiledProgram activeIsolates == 0 temiz");

    std::printf("%s (%d hata)\n", failures == 0 ? "TUM TESTLER GECTI" : "BASARISIZ", failures);
    return failures == 0 ? 0 : 1;
#endif
}
