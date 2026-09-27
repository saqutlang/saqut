// #295 — docs/compiler-errors.md üreticisi.
//
// Tanı ve çalışma zamanı hata kodlarının tek kaynağı src/diagnostic/diagnostic.hpp
// tablolarıdır (diagnosticCatalog, runtimeErrorCatalog). Bu program onları
// Markdown olarak basar; belge elle düzenlenmez.
//
//   ./build/gen_diagnostic_docs > docs/compiler-errors.md        (yeniden üret)
//   ./build/gen_diagnostic_docs --check docs/compiler-errors.md  (CTest: bayat mı?)
#include "diagnostic/diagnostic.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Markdown tablo hücresi: '|' tabloyu böler, kaçırılır.
std::string cell(const char* text) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (*p == '|') out += "\\|";
        else out += *p;
    }
    return out;
}

const char* levelName(DiagLevel level) {
    switch (level) {
        case DiagLevel::Error:   return "hata";
        case DiagLevel::Warning: return "uyarı";
        case DiagLevel::Note:    return "not";
        case DiagLevel::Hint:    return "ipucu";
    }
    return "";
}

std::string render() {
    std::ostringstream md;
    md << "# saQut tanı ve hata kodları\n\n"
       << "<!-- Bu dosya ÜRETİLİR: src/diagnostic/diagnostic.hpp → "
          "tests/gen_diagnostic_docs.cpp. Elle düzenlemeyin; yeniden üretmek için:\n"
       << "     ./build/gen_diagnostic_docs > docs/compiler-errors.md -->\n\n"
       << "Kodlar kararlıdır: `saqut check` JSONL çıktısı, LSP ve `Error.code` "
          "bu kodları taşır. Mesaj metni bağlama göre değişir; aşağıdaki başlık "
          "ve açıklama kodun genel anlamıdır.\n\n"
       << "## Derleme zamanı tanıları\n\n"
       << "| Kod | Seviye | Başlık | Açıklama |\n"
       << "|-----|--------|--------|----------|\n";
    for (const DiagInfo& d : diagnosticCatalog())
        md << "| " << d.code << " | " << levelName(d.level) << " | " << cell(d.title) << " | "
           << cell(d.explanation) << " |\n";

    md << "\n## Çalışma zamanı hata kodları (`Error.code`)\n\n"
       << "Program `try { ... } catch (Error e) { ... }` ile yakalayıp `e.code`'a "
          "bakabilir. Yakalanmayan hata programı çıkış kodu 70 ile bitirir.\n\n"
       << "| Kod | Başlık | Açıklama |\n"
       << "|-----|--------|----------|\n";
    for (const RuntimeErrorInfo& r : runtimeErrorCatalog())
        md << "| `" << r.code << "` | " << cell(r.title) << " | " << cell(r.explanation) << " |\n";

    md << "\nKodu olmayan ölümcül çalışma zamanı durumu: bütün thread'ler "
          "bloklandığında (deadlock) \"runtime error: all threads are blocked "
          "(deadlock)\" ve her thread'in beklediği yer basılır; çıkış kodu 70.\n";
    return md.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string doc = render();
    if (argc == 3 && std::string(argv[1]) == "--check") {
        std::ifstream in(argv[2], std::ios::binary);
        std::stringstream current;
        current << in.rdbuf();
        if (current.str() == doc) return 0;
        std::cerr << argv[2] << " bayat: tanı tablosu değişmiş.\n"
                  << "yeniden üret: ./build/gen_diagnostic_docs > docs/compiler-errors.md\n";
        return 1;
    }
    std::cout << doc;
    return 0;
}
