# #291 — CLI: komut bilgisi 5 yerde elle kopya

**Görev (tek cümle):** Bir CLI komutunun adı, kullanım satırı, konumsal argüman
sayısı ve kabul ettiği bayraklar tek bir tanımda dursun; `parseArgs`, yardım
metni ve dispatch bu tanımdan türesin.

## Doğrulanan durum (`1.0.1` @ df15a5c)

| Bilgi | Bugün nerede | Kopya |
|---|---|---|
| komut adı + açıklama + fonksiyon | `src/main.cpp:44-82` `registerCommand` | 1 |
| komut adı (ikinci kez) | `src/cli/args.hpp:88-91` `isKnownCommand` | 2 |
| konumsal sayı kuralı | `src/cli/args.hpp:263` `noPositional = lsp \|\| dap` | 3 |
| `--` ipucu verilen komutlar | `src/cli/args.hpp:268` `run/exec/bench` | 4 |
| `exec` eksik argüman mesajı | `src/cli/args.hpp:272` | 5 |
| kullanım satırı | `src/cli/cli.hpp:126-138` `commandUsage` `if` zinciri | 6 |
| bayrak yardımı + geçerli komutlar | `src/cli/cli.hpp:94-111` elle metin | 7 |
| bayrak ayrıştırma | `src/cli/args.hpp:96-257` `if` zinciri | 8 |

Bayrakların gerçek tüketicileri (`grep args.<alan> src/cli/commands`):

| Komut | Okuduğu alanlar |
|---|---|
| run | optimize, useJit, verbose, profile, gcStats, gcThreshold, programArgs (+ `gMaxCallDepth`) |
| exec | useJit, programArgs (+ `gMaxCallDepth`) |
| bench | useJit, verbose, benchRuns, compileOnly, programArgs (+ `gMaxCallDepth`) |
| ast | optimize, jsonOutput, outputFile |
| ir | optimize, showCfg |
| symbols | jsonOutput, jsonlOutput, compact |
| tokens, check, lsp, dap | — (lsp/dap `--stdio` no-op) |

Uyuşmazlıklar:
- Yardım `--json` için yalnız `(ast)` diyor; `symbols` de `jsonOutput` okuyor.
- Yardım `--compact`'ı `(symbols)` diyor; doğru.
- `saqut tokens e.sqt --jit --runs=3 --gc-stats` → exit 0, sessiz kabul.

Ölü / bayat kod:
- `CliArgs::stdinMode` hiçbir yerde `true` yapılmıyor (`-` artık kullanım
  hatası) → `readSource`/`inputFilePath` içindeki stdin dalı ve TODO ölü.
- `CliArgs::useJit` yorumu "desteklenmeyen bir şey görülürse VM'e düşer" diyor;
  aktif yol düşmüyor (knowledge-base §21).
- `src/main.cpp:8-19` KULLANIM listesi ve "YENİ KOMUT EKLEMEK İÇİN" adımları
  eksik; `src/cli/cli.hpp:20-21` "cli.hpp tarafından include edilir" yanlış.

Korunan davranış (tracked testler): `tests/general/exit_code_usage_error_test.sh`,
`cli_dead_option_removal_test.sh` (`run --optimized` no-op kalmalı),
`ir_cfg_flag_test.sh`/`ir_tty_color_test.sh` (`file:` öneki kullanıyor).

## Önerilen tasarım

```cpp
// src/cli/commands/tokens.hpp — komut kendi tanımını taşır
inline const CliCommand kTokensCommand{
    .name        = "tokens",
    .usage       = "saqut tokens <file>",
    .description = "print token list",
    .positional  = {1, 1},          // min, max
    .flags       = 0,               // kabul ettiği bayraklar (bit maskesi)
    .run         = cmdTokens,
};

// src/cli/command_list.hpp — tek kayıt listesi
inline const std::vector<const CliCommand*>& allCommands() {
    static const std::vector<const CliCommand*> list = {
        &kRunCommand, &kTokensCommand, /* ... */
    };
    return list;
}
```

- Bayraklar tek tabloda: ad, değer alıp almadığı, yardım metni, `CliArgs`'a
  yazan küçük fonksiyon. Yardımdaki "(run, exec, bench)" parantezi komut
  tanımlarından türetilir.
- `parseArgs` komutu listeden bulur; konumsal sayıyı ve bayrak geçerliliğini
  komut tanımına göre denetler. `isKnownCommand`, `noPositional`,
  `commandUsage` ve elle yazılmış yardım bloğu kalkar.
- Yeni komut = yeni dosya + `command_list.hpp`'de bir satır.

## Ürün sahibi kararları (2026-09-26)

1. Kayıt modeli: **açık liste** (`src/cli/command_list.hpp`).
2. Komutun kabul etmediği seçenek: **kullanım hatası 64**.
3. Eski `file:` / `output:` / `ast:` önekleri: **kaldırıldı**.

## Sonuç — DoD: Uygulandı (Test Edildi kabulü ürün sahibinde)

Dal `issue-291-cli-komut-kaydi` (taban `1.0.1` @ df15a5c), commit edilmedi.

**Yeni komut eklemek:** 1 dosya (`cmdX` + `kXCommand` kartı) + `command_list.hpp`'de
2 satır. Önce 5 yer + gizli `noPositional` adımı gerekiyordu. Rehber adımlarıyla
geçici bir `saqut mcp` eklenip denendi (yardımda göründü, `mcp dosya` → 64,
`mcp --jit` → 64), sonra geri alındı.

**Yeni seçenek eklemek:** `args.hpp`'de enum biti + `CliArgs` alanı + tablo
satırı, komut kartına bit. Yardım parantezi kendiliğinden türer. Yanlış yazılmış
bit adı derleme hatasıdır.

Değişenler:
- `src/cli/args.hpp`: `CliCommand`, `CliOption`, `cliOptions()`,
  `cliRemovedOptions()`; `parseArgs(argc, argv, commands)` kartlara göre denetler.
  `isKnownCommand`, `noPositional`, ölü `stdinMode`/stdin TODO dalı kaldırıldı.
- `src/cli/cli.hpp`: `CliDispatcher` sınıfı yerine `printHelp`/`dispatch`;
  elle yazılmış yardım bloğu ve `commandUsage` kaldırıldı.
- `src/cli/command_list.hpp` (yeni), 10 komut dosyasına kart, `main.cpp` sadeleşti.
- `symbols.hpp`: `--json` özel reddi kalktı; genel mesaj geçerli seçenekleri
  sayar (`valid: --jsonl, --compact`).
- `file:` kullanan koşucular/testler düz yola çevrildi: `cmake/run_golden*.cmake`,
  `cmake/run_differential.cmake`, `tests/harness/stage_harness.sh`,
  `tests/bench/perf_runner.sh`, `tests/general/ir_{cfg_flag,tty_color}_test.sh`.
- Belgeler: `readme.md`, `docs/project-overview.md`, `knowledge-base/06_Tooling.md`
  §34, `docs/learn/04-cli-komutu-ekleme.md` (ana ağaçta, izlenmeyen dosya).

Kullanıcıya görünen davranış değişiklikleri:
- Komutun kabul etmediği seçenek → 64 (`tokens --jit`, `check --dont-optimize`,
  `check f -- a` ...).
- `file:x.sqt` artık dosya yolu değil (modül bulunamadı hatası).
- Değer alan seçenekler `--runs 5` biçimini de kabul eder; `-o` değersizse 64
  (eskiden sessizce yok sayılıyordu); `--jit=1` → "takes no value".
- `--format` mesajı `option '--format' was removed: no command consumed it`
  biçimine geçti (exit 64 aynı).
- Yardımda `--verbose` satırı `-v` takma adını da gösteriyor; başka fark yok
  (baseline yardım çıktısıyla `diff` alındı).

Kanıt (Debug build, worktree):
- `cmake --build build`: uyarısız.
- `ctest -j8`: 346/348 geçti. Düşen `lsp_28_workspace_multi`,
  `lsp_29_editor_features` baseline ikiliyle de düşüyor: df15a5c lens başlığını
  `Referanslar: N` yaptı, beklenen JSONL güncellenmemiş. Bu işle ilgisiz.
- Yeni `tests/general/cli_command_options_test.sh` (CTest `cli_command_options`):
  baseline ikiliye karşı ilk senaryoda düşüyor (`tokens --jit` exit 0), yeni
  ikiliyle geçiyor.

Kanıtlanmayanlar / açık kalanlar:
- Release build ve `--jit` diferansiyel dışındaki platformlar denenmedi.
- VS Code eklentisi gerçek editörde denenmedi; yalnız `lsp --stdio` kabul
  edildiği test edildi.
- `--compact` `symbols`'ta hâlâ etkisiz (yalnız `--jsonl` ile birlikte reddediliyor);
  kaldırılması ayrı bir karar.
- `saqutwebside/` belgeleri taranmadı (ayrı repo).
