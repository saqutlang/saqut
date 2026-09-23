// ============================================================================
// saQut CLI — tokens komutu
//
// #146 (SQ-100-TOKEN-POSITIONS): her token satırı tür + lexeme + konum taşır.
//   Format: [TYPE] "lexeme"  FILE:LINE:COL  byteOffset=B byteLength=L
//
//   Tabanlar (kanonik):
//     file        : kaynak dosya yolu (SourceLocation.filePath)
//     line        : 1-tabanlı satır numarası
//     column      : 1-tabanlı UTF-8 BYTE-tabanlı kolon
//                   (offset - satır başı offset + 1; çok baytlı bir karakter
//                   görüntüde tek kolon kaplar ama BYTE kolonunda birden çok
//                   kolon sayılır — byte ve görüntü kolonu KARIŞTIRILMAZ)
//     byteOffset  : 0-tabanlı UTF-8 bayt offset'i (dosya başından; Token.start)
//     byteLength  : lexeme'nin bayt uzunluğu (Token.end - Token.start; UTF-8
//                   çok baytlı dizilerde bayt sayısı karakter sayısı değildir)
//   Token türü (gettype) ve lexeme (token) KORUNUR. Sıra kaynak sırasıdır
//   (deterministik); aynı girdi → bayt-bayt aynı çıktı.
// ============================================================================

#ifndef SAQUT_CLI_TOKENS
#define SAQUT_CLI_TOKENS

#include <iostream>
#include "cli/args.hpp"
#include "tokenizer/tokenizer.hpp"

inline int cmdTokens(const CliArgs& args) {
    std::string source = readSource(args);
    if (source.empty()) return 1;

    Tokenizer tokenizer;
    auto tokens = tokenizer.scan(source, inputFilePath(args));

    std::cout << "Tokens (" << tokens.size() << "):\n";
    for (auto* t : tokens) {
        std::cout << "  [" << t->gettype() << "] \"" << t->token << "\"  "
                  << t->loc.toString() << "  byteOffset=" << t->start
                  << " byteLength=" << (t->end - t->start) << "\n";
    }

    for (auto* t : tokens) delete t;
    return 0;
}

#endif // SAQUT_CLI_TOKENS
