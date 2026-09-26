// ============================================================================
// saQut Compiler — CLI Argüman Ayrıştırıcı ve Kaynak Okuma
// ============================================================================
//
// DİZİN:   src/cli/args.hpp
// BAĞIMLI: cli/exit_codes.hpp, core/config.hpp
//
// CLI'ın iki tablosu ve onları okuyan ayrıştırıcı:
//
//   CliCommand   — bir komutun tek tanımı: ad, kullanım satırı, açıklama,
//                  konumsal argüman sayısı, kabul ettiği seçenekler, fonksiyon.
//                  Her komut kendi dosyasında (cli/commands/) bir tane tanımlar;
//                  komut listesi cli/command_list.hpp'dedir.
//   cliOptions() — bütün seçeneklerin (--jit, --runs=N, ...) tek tablosu:
//                  ad, yardım metni ve değeri CliArgs'a yazan fonksiyon.
//
// parseArgs komutu listeden bulur; konumsal argüman sayısını ve her seçeneğin
// o komutta geçerli olup olmadığını komut tanımına göre denetler. Yardım
// metni (cli/cli.hpp) aynı iki tablodan türetilir; elle senkron liste yoktur.
//
// Kullanım hatası (64) sessizce yutulmaz (#257, #291): tanınmayan ya da
// komutun kabul etmediği seçenek, bozuk sayısal değer, fazladan/eksik
// konumsal argüman. Değer alan seçenekler `--runs=5` ve `--runs 5`
// biçimlerinin ikisini de kabul eder.
//
// YENİ SEÇENEK: CliOption'a bir bit, CliArgs'a bir alan, cliOptions()'a bir
// satır; seçeneği kabul eden komutların `options` alanına bit.
//
// ============================================================================

#ifndef SAQUT_CLI_ARGS
#define SAQUT_CLI_ARGS

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "cli/exit_codes.hpp"
#include "core/config.hpp"

struct CliArgs {
    std::string command;
    std::vector<std::string> positional;
    std::string outputFile;    // -o / --output=<file>
    bool showHelp    = false;
    bool compact     = false;  // --compact: boşluksuz JSON
    // Optimizasyon VARSAYILAN OLARAK AÇIKTIR (sabit katlama + ölü kod eleme).
    // --dont-optimize kapatır. Gerekçe: production koşuları optimize edilmiş
    // derlemeyi kullanır; varsayılanın onunla aynı olması, geliştirmede
    // görülen davranışın dağıtılan davranış olmasını garantiler. (Optimizasyon
    // gözlenen çıktıyı değiştirmez — ADR-038; tests/run.sh "optimizasyon
    // sonucu degistirmiyor" gate'i bunu her fixture'da doğrular.)
    bool optimize    = true;   // --dont-optimize ile false olur
    bool jsonOutput  = false;  // --json: JSON çıktı üret (varsayılan: düz metin)
    bool jsonlOutput = false;  // --jsonl: canonical JSONL çıktı (SQ-100 ailesi, #145)
    bool showCfg     = false;  // --cfg: saqut ir — flat liste yerine CFG (BasicBlock + kenar) bas
    int  benchRuns   = 5;      // --runs=N: benchmark tekrar sayısı
    bool compileOnly = false;  // --compile-only: VM çalıştırmasını atla
    bool verbose     = false;  // --verbose: her aşamanın bitişini canlı yaz
    int  gcThreshold = 0;      // --gc-threshold=N: toplama eşiği BAYT (0 = varsayılan, negatif = toplama kapalı)
    bool gcStats     = false;  // --gc-stats: koşu sonunda GC istatistiklerini stderr'e yaz

    // --jit: MIR JIT backend'i [EXPERIMENTAL] (ADR-042). Programın tamamını
    // derleyemezse açık hatayla reddeder; VM'e sessizce düşmez.
    bool useJit = false;

    // src/profiling/: --profile — token/parser/ir-gen/vm-veya-jit
    // aşamalarını ayrı ayrı ölçüp stderr'e yazdırır (saqut run).
    bool profile = false;

    // `--` sonrası argümanlar — sys::args() ile programa geçilir.
    std::vector<std::string> programArgs;

    // Boş değilse ayrıştırma bir kullanım hatası buldu; main 64 ile çıkar.
    std::string usageError;
};

// Sayısal bayrak değeri: tamamı tamsayı değilse false (`--runs=abc`, `--runs=5x`).
inline bool parseIntFlag(const std::string& text, int& out) {
    if (text.empty()) return false;
    try {
        size_t used = 0;
        int v = std::stoi(text, &used);
        if (used != text.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// CliOption — seçenek kimlikleri (bit maskesi)
// ============================================================================
// Bir komutun kabul ettiği seçenekler CliCommand::options'ta bu bitlerin
// birleşimidir (`OPT_JIT | OPT_VERBOSE`). Yanlış yazılmış bir ad derleme
// hatasıdır; string listesi olsaydı sessizce hiçbir seçeneği eşlemezdi.
enum CliOption : unsigned {
    OPT_JIT            = 1u << 0,
    OPT_DONT_OPTIMIZE  = 1u << 1,   // geriye uyum no-op'u --optimized de bu bit
    OPT_VERBOSE        = 1u << 2,
    OPT_PROFILE        = 1u << 3,
    OPT_GC_STATS       = 1u << 4,
    OPT_GC_THRESHOLD   = 1u << 5,
    OPT_MAX_CALL_DEPTH = 1u << 6,
    OPT_RUNS           = 1u << 7,
    OPT_COMPILE_ONLY   = 1u << 8,
    OPT_JSON           = 1u << 9,
    OPT_JSONL          = 1u << 10,
    OPT_COMPACT        = 1u << 11,
    OPT_CFG            = 1u << 12,
    OPT_OUTPUT         = 1u << 13,
    OPT_PROGRAM_ARGS   = 1u << 14,  // `--`: kalan argümanlar sys::args()'a (#90)
    OPT_STDIO          = 1u << 15,  // no-op: LSP/DAP istemcilerinin taşıma bayrağı
};

// ============================================================================
// CliCommand — bir komutun tek tanımı
// ============================================================================
// Örnek (cli/commands/tokens.hpp):
//   inline constexpr CliCommand kTokensCommand{
//       .name = "tokens", .usage = "saqut tokens <file>",
//       .description = "print token list", .run = cmdTokens};
// Alanlar tanım sırasıyla yazılır (C++20 designated initializer kuralı);
// varsayılanı uyan alan yazılmaz.
struct CliCommand {
    const char* name;
    const char* usage;                          // yardımdaki kullanım satırı
    const char* description;                    // yardımdaki açıklama
    int         minPositional = 1;              // konumsal argüman (dosya yolu) sayısı
    int         maxPositional = 1;
    unsigned    options       = 0;              // kabul ettiği CliOption bitleri
    int (*run)(const CliArgs&) = nullptr;
    const char* missingArgument = "no input file";  // minPositional karşılanmazsa
    bool        hidden        = false;          // true: yardımda listelenmez
};

// ============================================================================
// CliOptionSpec — seçenek tablosunun bir satırı
// ============================================================================
struct CliOptionSpec {
    CliOption   bit;
    const char* name;       // "--jit"
    const char* alias;      // kısa ad ("-v") ya da nullptr
    const char* valueName;  // nullptr: değer almaz; "N": --runs=N ya da --runs N
    const char* help;       // yardım metni ('\n' ile satır bölünür); nullptr: gizli
    // Değeri CliArgs'a yazar. Başarıda nullptr, bozuk değerde beklenen biçimin
    // tarifini döndürür ("a positive integer").
    const char* (*apply)(CliArgs& args, const std::string& value);
};

inline const std::vector<CliOptionSpec>& cliOptions() {
    // Yardımdaki "Command options" sırası bu tablonun sırasıdır.
    static const std::vector<CliOptionSpec> table = {
        {OPT_JIT, "--jit", nullptr, nullptr, "Use the MIR JIT backend",
         [](CliArgs& a, const std::string&) -> const char* { a.useJit = true; return nullptr; }},
        {OPT_DONT_OPTIMIZE, "--dont-optimize", nullptr, nullptr,
         "Disable constant folding + dead code elim.",
         [](CliArgs& a, const std::string&) -> const char* { a.optimize = false; return nullptr; }},
        // --optimized artık varsayılan davranıştır; mevcut betikler kırılmasın
        // diye --dont-optimize'ı kabul eden komutlarda no-op olarak kalır.
        {OPT_DONT_OPTIMIZE, "--optimized", nullptr, nullptr, nullptr,
         [](CliArgs&, const std::string&) -> const char* { return nullptr; }},
        {OPT_VERBOSE, "--verbose", "-v", nullptr, "Print stage progress",
         [](CliArgs& a, const std::string&) -> const char* { a.verbose = true; return nullptr; }},
        {OPT_PROFILE, "--profile", nullptr, nullptr, "Report per-stage timings",
         [](CliArgs& a, const std::string&) -> const char* { a.profile = true; return nullptr; }},
        {OPT_GC_STATS, "--gc-stats", nullptr, nullptr, "Print GC statistics on exit",
         [](CliArgs& a, const std::string&) -> const char* { a.gcStats = true; return nullptr; }},
        {OPT_MAX_CALL_DEPTH, "--max-call-depth", nullptr, "N",
         "Maximum call depth (default 100000)",
         [](CliArgs&, const std::string& v) -> const char* {
             int depth = 0;
             if (!parseIntFlag(v, depth) || depth < 1) return "a positive integer";
             gMaxCallDepth = depth;
             return nullptr;
         }},
        {OPT_GC_THRESHOLD, "--gc-threshold", nullptr, "N",
         "GC threshold in bytes; 0 = default,\nnegative disables collection",
         [](CliArgs& a, const std::string& v) -> const char* {
             return parseIntFlag(v, a.gcThreshold) ? nullptr : "an integer";
         }},
        {OPT_RUNS, "--runs", nullptr, "N", "Benchmark iterations (default 5)",
         [](CliArgs& a, const std::string& v) -> const char* {
             if (!parseIntFlag(v, a.benchRuns) || a.benchRuns < 1) return "a positive integer";
             return nullptr;
         }},
        {OPT_COMPILE_ONLY, "--compile-only", nullptr, nullptr, "Skip execution, compile only",
         [](CliArgs& a, const std::string&) -> const char* { a.compileOnly = true; return nullptr; }},
        {OPT_JSON, "--json", nullptr, nullptr, "JSON output",
         [](CliArgs& a, const std::string&) -> const char* { a.jsonOutput = true; return nullptr; }},
        {OPT_JSONL, "--jsonl", nullptr, nullptr, "Canonical JSONL output",
         [](CliArgs& a, const std::string&) -> const char* { a.jsonlOutput = true; return nullptr; }},
        {OPT_COMPACT, "--compact", nullptr, nullptr, "Whitespace-free JSON",
         [](CliArgs& a, const std::string&) -> const char* { a.compact = true; return nullptr; }},
        {OPT_CFG, "--cfg", nullptr, nullptr, "Print CFG instead of a flat list",
         [](CliArgs& a, const std::string&) -> const char* { a.showCfg = true; return nullptr; }},
        {OPT_OUTPUT, "--output", "-o", "<file>", "Write output to a file (with --json)",
         [](CliArgs& a, const std::string& v) -> const char* { a.outputFile = v; return nullptr; }},
        // `--` parseArgs'ta özel işlenir (kalan her şeyi programArgs'a alır);
        // bu satır yalnız geçerlilik denetimi ve yardım içindir.
        {OPT_PROGRAM_ARGS, "--", nullptr, nullptr, "Pass remaining args to the program\nvia sys::args()",
         [](CliArgs&, const std::string&) -> const char* { return nullptr; }},
        {OPT_STDIO, "--stdio", nullptr, nullptr, nullptr,
         [](CliArgs&, const std::string&) -> const char* { return nullptr; }},
    };
    return table;
}

// Kaldırılmış seçenekler: eski betikler geçebilir — neden reddedildiğini söyle.
// `prefix` ile başlayan her argüman eşleşir (`--allow` → `--allow-fs`, ...).
struct CliRemovedOption {
    const char* prefix;
    const char* reason;
};

inline const std::vector<CliRemovedOption>& cliRemovedOptions() {
    static const std::vector<CliRemovedOption> table = {
        {"--format", "no command consumed it"},
        {"--allow", "the capability system no longer exists (ADR-043); host calls are open by default"},
        {"--capabilities", "the capability system no longer exists (ADR-043); host calls are open by default"},
    };
    return table;
}

inline const CliOptionSpec* findCliOption(const std::string& name) {
    for (const CliOptionSpec& opt : cliOptions())
        if (name == opt.name || (opt.alias && name == opt.alias)) return &opt;
    return nullptr;
}

inline const CliCommand* findCliCommand(const std::vector<const CliCommand*>& commands,
                                        const std::string& name) {
    for (const CliCommand* cmd : commands)
        if (name == cmd->name) return cmd;
    return nullptr;
}

// Komutun kabul ettiği (yardımda görünen) seçeneklerin adları: "--jsonl, --compact".
inline std::string cliOptionNames(unsigned options) {
    std::string names;
    for (const CliOptionSpec& opt : cliOptions()) {
        if (!(options & opt.bit) || !opt.help) continue;
        if (!names.empty()) names += ", ";
        names += opt.name;
    }
    return names;
}

// ============================================================================
// parseArgs
// ============================================================================
inline CliArgs parseArgs(int argc, char* argv[], const std::vector<const CliCommand*>& commands) {
    CliArgs args;
    // Görülen seçenekler; komut belli olduktan sonra geçerlilikleri denetlenir.
    std::vector<const CliOptionSpec*> seen;
    // İlk hata kazanır: kullanıcı en baştaki sorunu görür.
    auto fail = [&args](const std::string& message) {
        if (args.usageError.empty()) args.usageError = message;
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "--") {
            seen.push_back(findCliOption("--"));
            for (int j = i + 1; j < argc; j++)
                args.programArgs.push_back(argv[j]);
            break;
        }
        if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
            continue;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "saQut " << SAQUT_VERSION << "\n";
            std::exit(saqut::exit_code::kSuccess);
        }
        if (arg == "-") {
            // Kaynaktan okuma yalnız dosya yoluyla: modül çözümlemesi
            // import yollarını dosyanın dizinine göre çözer (bkz. modules).
            fail("reading the program from standard input is not supported; pass a file path");
            continue;
        }

        if (arg.size() > 1 && arg[0] == '-') {
            std::string name = arg;
            std::string value;
            bool hasValue = false;
            if (const size_t eq = arg.find('='); eq != std::string::npos) {
                name     = arg.substr(0, eq);
                value    = arg.substr(eq + 1);
                hasValue = true;
            }

            bool removed = false;
            for (const CliRemovedOption& r : cliRemovedOptions()) {
                if (name.starts_with(r.prefix)) {
                    fail("option '" + name + "' was removed: " + r.reason);
                    removed = true;
                    break;
                }
            }
            if (removed) continue;

            const CliOptionSpec* opt = findCliOption(name);
            if (!opt) {
                // Tanınmayan seçenek sessizce konumsal argüman ya da modül adı
                // sayılmaz (eskiden `--gc-treshold=5` "cannot open module" veriyordu).
                fail("unknown option '" + arg + "'");
                continue;
            }
            if (opt->valueName && !hasValue) {
                if (i + 1 >= argc) {
                    fail("option '" + name + "' requires a value (" + name + "=" + opt->valueName + ")");
                    continue;
                }
                value = argv[++i];
            } else if (!opt->valueName && hasValue) {
                fail("option '" + name + "' takes no value");
                continue;
            }

            seen.push_back(opt);
            if (const char* expected = opt->apply(args, value))
                fail("invalid value for " + name + ": '" + value + "' (expected " + expected + ")");
            continue;
        }

        // İlk argüman komut mu? Değilse ve bir dosyaya benziyorsa `run`
        // kısayolu (`saqut prog.sqt`); ikisi de değilse bilinmeyen komut.
        if (i == 1) {
            if (arg == "help") {
                args.showHelp = true;
                continue;
            }
            if (findCliCommand(commands, arg)) {
                args.command = arg;
                continue;
            }
            std::ifstream probe(arg);
            const bool looksLikeFile = probe.good() || arg.ends_with(".sqt");
            if (!looksLikeFile) {
                fail("unknown command '" + arg + "'");
                continue;
            }
            args.command = "run";
        }

        args.positional.push_back(arg);
    }

    if (args.command.empty()) args.command = "run";
    if (args.showHelp || !args.usageError.empty()) return args;

    const CliCommand* cmd = findCliCommand(commands, args.command);
    if (!cmd) {
        fail("unknown command '" + args.command + "'");
        return args;
    }

    // Seçenek bu komutta geçerli mi? Anlamsız seçenek sessizce kabul edilmez
    // (`saqut tokens a.sqt --jit` eskiden exit 0 veriyordu, #291).
    for (const CliOptionSpec* opt : seen) {
        if (cmd->options & opt->bit) continue;
        const std::string valid = cliOptionNames(cmd->options);
        fail("option '" + std::string(opt->name) + "' is not valid for '" + cmd->name + "'" +
             (valid.empty() ? " (it takes no options)" : " (valid: " + valid + ")"));
        return args;
    }

    // Konumsal argüman sayısı. Fazlası sessizce düşmez: program argümanı
    // olmaları muhtemeldir ve `--` sonrasına gitmeleri gerekir.
    const int count = static_cast<int>(args.positional.size());
    if (count > cmd->maxPositional) {
        const std::string& extra = args.positional[cmd->maxPositional];
        std::string message = "unexpected argument '" + extra + "'";
        if (cmd->options & OPT_PROGRAM_ARGS)
            message += " (program arguments go after '--': saqut " + args.command + " <file> -- " +
                       extra + ")";
        fail(message);
    } else if (count < cmd->minPositional) {
        fail(cmd->missingArgument);
    }

    return args;
}

// ============================================================================
// readSource: ilk konumsal argümandaki dosyayı oku (okunamazsa boş string)
// ============================================================================
inline std::string readSource(const CliArgs& args) {
    if (args.positional.empty()) return "";

    std::string path = args.positional[0];
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "error: cannot open file '" << path << "'\n";
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ============================================================================
// inputFilePath: kaynak dosyanın yolu (konumsal argüman yoksa boş string)
// ============================================================================
inline std::string inputFilePath(const CliArgs& args) {
    if (args.positional.empty()) return "";
    return args.positional[0];
}

#endif // SAQUT_CLI_ARGS
