// ============================================================================
// saQut CLI — exec komutu (IR göster + çalıştır)
// ============================================================================

#ifndef SAQUT_CLI_EXEC
#define SAQUT_CLI_EXEC

// ============================================================================
// saQut CLI — exec komutu
//
// Kullanım: saqut exec "1 + 2"
//
// İfadeyi int main() { print(<expr>); return 0; } olarak sarmalar,
// tam derleme pipeline'ından geçirir ve sonucu stdout'a yazar.
// ============================================================================

#include <iostream>
#include <string>
#include "cli/args.hpp"
#include "cli/exit_codes.hpp"
#include "tokenizer/tokenizer.hpp"
#include "parser/parser.hpp"
#include "symbol/symbol_table.hpp"
#include "symbol/symbol_collector.hpp"
#include "semantic/type_checker.hpp"
#include "semantic/structural_validator.hpp"
#include "diagnostic/diagnostic_engine.hpp"
#include "ir/ir_generator.hpp"
#include "vm/interpreter.hpp"
#include "mir/mir_backend.hpp"
#include "runtime/compiled_program.hpp"
#include "runtime/isolate.hpp"

// startsWithStatement — Kullanıcı girdisinin bir DEYİM mi (for/while/if/
// değişken tanımı ...) yoksa bir İFADE mi (call/literal/aritmetik ...) ile
// başladığını hafif bir ön-parse ("probe") ile saptar.
//
// GEREKÇE: exec, tarihsel olarak girdiyi print(...) içine sarar (int main() {
// print(<expr>); ... }). Bu, `1 + 2` gibi ifadelerde doğru ama `for(...){...}`
// gibi deyimlerde print(for(...)) üretip sözdizimi hatası verir. Bu yüzden önce
// girdiyi olduğu gibi bir gövdeye koyup parse eder, main gövdesinin İLK
// statement'ının türüne bakarız:
//   - ExpressionStatement  → ifade  → print(...) ile sarılmalı (true DEĞİL)
//   - diğer her şey (For/While/If/VariableDecl/DoWhile/Switch/Try ...) → deyim
//     → sarılmamalı (true)
// Belirsiz / parse edilemeyen durumda `false` döner → eski (print ile sar)
// davranışı korunur (geri uyumluluk).
//
// Probe kendi yerel DiagnosticEngine'ini kullanır ki başarısız parse'ta
// stderr'e gürültü basmasın (gerçek hata, asıl pipeline'da raporlanır).
inline bool startsWithStatement(const std::string& input) {
    const std::string probeSource = "int main() {\n" + input + ";\n}\n";

    Tokenizer tokenizer;
    auto      tokens = tokenizer.scan(probeSource, "<exec-probe>");

    DiagnosticEngine probeDiag;  // hataları yutar, cerr'e basmaz
    Parser           parser(&probeDiag);
    ASTNode*         ast = parser.parse(tokens);

    bool isStatement = false;
    if (ast && !probeDiag.hasErrors()) {
        // Program → FunctionDecl(main) → Block(gövde) → ilk statement
        for (auto* top : ast->getChildren()) {
            if (top->kind != ASTKind::FunctionDecl) continue;
            for (auto* bodyChild : top->getChildren()) {
                if (bodyChild->kind != ASTKind::Block) continue;
                auto& stmts = bodyChild->getChildren();
                if (!stmts.empty()) {
                    // ExpressionStatement = ifade; gerisi = deyim.
                    isStatement = stmts[0]->kind != ASTKind::ExpressionStatement;
                }
                break;
            }
            break;
        }
    }

    delete ast;
    for (auto* t : tokens) delete t;
    return isStatement;
}

inline int cmdExec(const CliArgs& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: saqut exec \"<expression>\"\n";
        std::cerr << "example: saqut exec \"1 + 2\"\n";
        return saqut::exit_code::kUsageError;
    }

    // Kullanıcının girdisini minimal programa sar. Deyimle başlıyorsa (for/while/
    // değişken tanımı ...) olduğu gibi çalıştır; ifadeyle başlıyorsa print(...) ile
    // sarıp değerini bastır (klasik `exec "1 + 2"` → 3 davranışı).
    const std::string& expr = args.positional[0];
    std::string        source;
    if (startsWithStatement(expr)) {
        source = "int main() {\n    " + expr + ";\n    return 0;\n}\n";
    } else {
        source = "int main() {\n    print(" + expr + ");\n    return 0;\n}\n";
    }
    const std::string syntheticPath = "<exec>";

    Tokenizer        tokenizer;
    auto             tokens = tokenizer.scan(source, syntheticPath);

    // #134: exec, run/check ile AYNI diagnostic kapısından geçmeli. Önceki
    // kod Parser'ı DiagnosticEngine vermeden (diag_ == nullptr) çağırıyordu;
    // bu modda Parser hatayı yalnız stderr'e basıp panic-mode kurtarma ile
    // devam ediyor ve yine de non-null bir AST döndürüyor — çağıran yalnız
    // `!ast`'i kontrol ettiği için parse hatası sessizce yutuluyor, hatalı
    // AST derlenip çalıştırılıyor, yanlış stdout + exit 0 üretiyordu
    // (ADR-038 ihlali). Gerçek DiagnosticEngine verip hasErrors()'ı run.hpp
    // ile aynı şekilde kontrol ediyoruz.
    DiagnosticEngine diag;
    Parser           parser(&diag);
    ASTNode*         ast = parser.parse(tokens);

    if (!ast || diag.hasErrors()) {
        diag.printAll(std::cerr);
        delete ast;
        for (auto* t : tokens) delete t;
        return saqut::exit_code::kDataError;
    }

    SymbolTable symbolTable;
    SymbolCollector(symbolTable, diag).collect(ast);

    if (!diag.hasErrors()) {
        TypeChecker(symbolTable, diag).check(ast);
        StructuralValidator(diag).validate(ast);
    }

    if (diag.hasErrors()) {
        diag.printAll(std::cerr);
        delete ast;
        for (auto* t : tokens) delete t;
        return saqut::exit_code::kDataError;
    }

    IRGenerator irGenerator;
    IRProgram   program = irGenerator.generate(ast, symbolTable, syntheticPath);
    // ADR-045 (1-e): derleme bitti; dosya kayıt defteri koşu boyunca salt okunur.
    FileRegistry::instance().freeze();

    int exitCode = 0;
    try {
        if (args.useJit) {
            int jitResult = 0;
            mir_backend::UnsupportedReason reason;
            // c4: CompiledProgram'ın sahibi komut; kapsam sonunda yıkılır.
            std::unique_ptr<CompiledProgram> compiled =
                mir_backend::compileProgram(program, reason);
            bool jitOk = compiled != nullptr;
            if (jitOk) {
                Isolate&     iso = Isolate::current();
                IsolateGuard guard(iso, compiled.get());
                mir_backend::runOnIsolate(*compiled, iso, jitResult, args.programArgs);
            }
            if (jitOk) {
                exitCode = jitResult;
            } else {
                std::cerr << "exec: --jit unsupported for expression '" << expr
                          << "' (function '" << reason.functionName
                          << "', opcode: " << reason.opcodeName << ")\n";
                exitCode = saqut::exit_code::kSoftwareError;
            }
        } else {
            Interpreter vm(program);
            vm.setProgramArgs(args.programArgs);
            exitCode = vm.run();
        }
    } catch (const std::exception& e) {
        std::cerr << "exec: runtime error: " << e.what() << "\n";
        exitCode = saqut::exit_code::kSoftwareError;
    }

    delete ast;
    for (auto* t : tokens) delete t;
    return exitCode;
}

#endif // SAQUT_CLI_EXEC
