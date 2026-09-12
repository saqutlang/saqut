cmake_minimum_required(VERSION 3.16)
project(saqut VERSION 0.9.7 LANGUAGES CXX C)

# Derleyici sürümünü koda #define olarak geçir
add_compile_definitions(SAQUT_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

# Default: Release (static, optimized, no debug)
# Use -DCMAKE_BUILD_TYPE=Debug for debug build
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
    message(STATUS "Debug modu (yavas, debug sembollu)")
else()
    add_compile_options(-O3 -DNDEBUG -fno-omit-frame-pointer)
    add_link_options(-static-libstdc++ -static-libgcc -static)
    message(STATUS "Release modu (statik, optimize)")
endif()

# ── Gömülü saQut çekirdeği (#226) ─────────────────────────────────────────
# src/internal/*.sqt gerçek saQut kaynaklarıdır ve derleme sırasında binary'ye
# gömülür (Node.js'in js2c.py deseni). Elle gömme YOK: yeni bir .sqt eklemek
# yeterlidir, CONFIGURE_DEPENDS sayesinde cmake onu kendiliğinden görür.
file(GLOB_RECURSE INTERNAL_SQT CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/internal/*.sqt")
set(INTERNAL_HEADER "${CMAKE_BINARY_DIR}/generated/internal_sources.hpp")

add_custom_command(
    OUTPUT  "${INTERNAL_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/embed_internal.py"
                    "${CMAKE_SOURCE_DIR}/src/internal"
                    "${INTERNAL_HEADER}"
    DEPENDS ${INTERNAL_SQT} "${CMAKE_SOURCE_DIR}/scripts/embed_internal.py"
    COMMENT "saQut cekirdegi gomuluyor (src/internal/*.sqt)"
    VERBATIM)
add_custom_target(embed_internal DEPENDS "${INTERNAL_HEADER}")

# Tüm kaynak dosyaları topla
file(GLOB_RECURSE SOURCES "src/*.cpp")

# ── MIR JIT backend (Fast-mode, #80, MIRPLAN.md) ──────────────────────────
# Yalnızca mir.c + mir-gen.c derlenir — c2mir (C->MIR derleyicisi) saQut'ta
# hiç kullanılmıyor (MIRPLAN.md §1: MIR C API üzerinden doğrudan kod
# üretiyoruz, metin/C kaynak yolu yok). mir-gen.c mimariyi kendi içinde
# preprocessor ile seçer (mir-gen-x86_64.c vb.) — ayrı arch dosyası eklemek
# gerekmez.
add_library(mir_core STATIC
    src/mir/vendor/mir.c
    src/mir/vendor/mir-gen.c
)
target_include_directories(mir_core PUBLIC src/mir/vendor)
target_compile_options(mir_core PRIVATE -std=gnu11 -Wno-abi -fsigned-char -fPIC)
include(CheckCCompilerFlag)
check_c_compiler_flag(-fno-tree-sra SAQUT_MIR_HAS_NO_TREE_SRA)
if(SAQUT_MIR_HAS_NO_TREE_SRA)
    target_compile_options(mir_core PRIVATE -fno-tree-sra)
endif()
check_c_compiler_flag(-fno-ipa-cp-clone SAQUT_MIR_HAS_NO_IPA_CP_CLONE)
if(SAQUT_MIR_HAS_NO_IPA_CP_CLONE)
    target_compile_options(mir_core PRIVATE -fno-ipa-cp-clone)
endif()

add_executable(saqut ${SOURCES})
# Üretilen başlık kaynaklardan önce hazır olmalı.
add_dependencies(saqut embed_internal)
target_include_directories(saqut PRIVATE "${CMAKE_BINARY_DIR}/generated")

target_include_directories(saqut PRIVATE src)
target_link_libraries(saqut PRIVATE mir_core)

# IR-K6 (#139): saqut ir dump'ının operand renderer'ı Opcode üzerinde
# default'suz bir switch'tir (src/ir/ir_function.cpp). Bu dosyada -Wswitch'i
# -Werror'a yükseltmek, yeni bir Opcode enum değeri eklenip renderer'a case
# eklenmezse build'i kırar — yeni opcode sessizce operandsız satıra düşemez.
# Yalnızca bu dosyaya uygulanır; projenin geri kalanındaki mevcut switch'leri
# etkilemez.
set_source_files_properties(src/ir/ir_function.cpp PROPERTIES
    COMPILE_OPTIONS "-Werror=switch"
)

# ── Testler ──────────────────────────────────────────────────────────────────
enable_testing()

set(SAQUT_BINARY "${CMAKE_BINARY_DIR}/saqut")
set(GOLDEN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/run_golden.cmake")

# Birim testleri (tests/run.sh)
add_test(
    NAME unit_tests
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run.sh"
)

# #156: kUsageError (64) tracked regresyon — run/check/ir "-" (stdin, henüz
# desteklenmiyor) ile çağrıldığında merkezi kullanım-hatası sınıfını verir.
add_test(
    NAME exit_code_usage_error
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/exit_code_usage_error_test.sh" "${SAQUT_BINARY}"
)

# #147: --format kaldırıldı (usage error 64), --output/canlı seçen matrisi
# korunuyor.
add_test(
    NAME cli_dead_option_removal
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/cli_dead_option_removal_test.sh"
            "${SAQUT_BINARY}" "${CMAKE_SOURCE_DIR}"
)

# #141: saqut ir yalnız gerçek TTY'de renkli; redirect/pipe'ta ANSI CSI yok.
add_test(
    NAME ir_tty_color
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/ir_tty_color_test.sh"
            "${SAQUT_BINARY}" "${CMAKE_SOURCE_DIR}"
)

# #134: saqut exec parser hatasını artık yutmuyor (run/check ile aynı kapı).
add_test(
    NAME exec_diagnostic_gate
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/exec_diagnostic_gate_test.sh"
            "${SAQUT_BINARY}"
)

# RG-7 (#157): saqut ast/symbols artık run/check/ir ile aynı diagnostic
# kapısından geçiyor (ast: hiç semantic kontrol yoktu; symbols: literal
# exit kodu kullanıyordu).
add_test(
    NAME ast_symbols_diagnostic_gate
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/ast_symbols_diagnostic_gate_test.sh"
            "${SAQUT_BINARY}"
)

# #170: parser'daki 25 kapanış-delimiter site'i ((), [], {}) artık E905
# diagnostic'i basıyor — eskiden sessizce yutulup program "başarıyla" (exit 0)
# çalıştırılıyordu.
add_test(
    NAME parser_closing_delimiter_diagnostic
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/parser_closing_delimiter_diagnostic_test.sh"
            "${SAQUT_BINARY}"
)

# #144 (SQ-100-CHECK-JSONL): saqut check stdout canonical JSONL —
# check.header → check.diagnostic* → check.end; sıra deterministik,
# exit 0/65 (kDataError).
add_test(
    NAME check_jsonl
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/check_jsonl_test.sh"
            "${SAQUT_BINARY}"
)

# #145 (SQ-100-SYMBOLS-JSONL): symbols varsayılan metin çıktısını korur;
# --jsonl makine yüzeyi (header → symbol/diagnostic* → end), eski --json
# preview kaldırıldı, --compact --jsonl ile anlamsız → exit 64.
add_test(
    NAME symbols_jsonl
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/symbols_jsonl_test.sh"
            "${SAQUT_BINARY}"
)

# #218: saqut ir --cfg — flat liste yerine CFG (BasicBlock + kenarlar)
# basar; fall-through kenar + blok ID semantiği + determinizm doğrulanır.
add_test(
    NAME ir_cfg_flag
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/ir_cfg_flag_test.sh"
            "${SAQUT_BINARY}" "${CMAKE_SOURCE_DIR}"
)

# #146 (SQ-100-TOKEN-POSITIONS): saqut tokens her token için file +
# line/column (1-tabanlı, column BYTE-tabanlı) + byteOffset (0-tabanlı) +
# byteLength taşır; ASCII + UTF-8 çok baytlı fixture, bayt-identical
# determinizm.
add_test(
    NAME tokens_positions
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/general/tokens_positions_test.sh"
            "${SAQUT_BINARY}"
)

# Golden testleri — tests/golden/**/*.sqt dosyalarını otomatik keşfet.
# Her .sqt için iki olası expected dosyası kontrol edilir:
#   BASE.expected      → saqut run  (siyah-kutu çıktı testi)
#   BASE.ir_opt.expected → saqut ir --optimized (beyaz-kutu IR testi)
file(GLOB_RECURSE ALL_GOLDEN_SQT CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/golden/*.sqt"
)
foreach(SQT_FILE ${ALL_GOLDEN_SQT})
    file(RELATIVE_PATH REL "${CMAKE_SOURCE_DIR}/tests/golden" "${SQT_FILE}")
    string(REPLACE ".sqt" "" BASE "${REL}")
    string(REPLACE "/" "_" TNAME "${BASE}")

    # ADR-036 (#76): BASE.flags — --allow-fs vb. gerektiren testler için ekstra
    # CLI bayrakları (satır başına bir bayrak). Yoksa boş.
    set(FLAGS_FILE "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.flags")
    set(EXTRA_ARGS_JOINED "")
    if(EXISTS "${FLAGS_FILE}")
        file(STRINGS "${FLAGS_FILE}" FLAG_LIST)
        list(JOIN FLAG_LIST "|" EXTRA_ARGS_JOINED)
    endif()

    # Siyah-kutu: saqut run çıktısı
    set(RUN_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.expected")
    if(EXISTS "${RUN_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${RUN_EXPECTED}
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                -P "${GOLDEN_SCRIPT}"
        )
    endif()

    # Beyaz-kutu: saqut ir --optimized çıktısı
    set(IR_OPT_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.ir_opt.expected")
    if(EXISTS "${IR_OPT_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}_ir_opt"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${IR_OPT_EXPECTED}
                -DCOMMAND=ir
                -DOPTIMIZED=1
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                -P "${GOLDEN_SCRIPT}"
        )
    endif()

    # Beyaz-kutu: saqut ir (plain, optimize edilmemiş) çıktısı. #139'un
    # Başmimar review'unda istenen "plain IR tracked regression" — daha önce
    # yalnız --optimized dump tracked'dı, optimize edilmemiş HeavyIR hiç
    # golden'a bağlanmamıştı.
    set(IR_PLAIN_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.ir_plain.expected")
    if(EXISTS "${IR_PLAIN_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}_ir_plain"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${IR_PLAIN_EXPECTED}
                -DCOMMAND=ir
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                -P "${GOLDEN_SCRIPT}"
        )
    endif()

    # Doğruluk kalkanı: saqut run --optimized çıktısı == beklenen golden değer
    # "Optimizasyon oldu" değil, "optimize IR doğru sonuç verdi" testleri.
    set(RUN_OPT_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.run_opt.expected")
    if(EXISTS "${RUN_OPT_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}_run_opt"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${RUN_OPT_EXPECTED}
                -DOPTIMIZED=1
                -P "${GOLDEN_SCRIPT}"
        )
    endif()

    # Diferansiyel test (#92): VM ile MIR JIT aynı fixture'ı koşar, stdout +
    # exit code bayt-bayt karşılaştırılır. JIT henüz desteklemiyorsa (kısmi
    # dilim kapsamı) test "Not Run" olarak atlanır — bkz. run_differential.cmake
    # başlığı + SKIP_REGULAR_EXPRESSION altında.
    if(EXISTS "${RUN_EXPECTED}")
        add_test(
            NAME "differential_${TNAME}"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                -P "${CMAKE_SOURCE_DIR}/cmake/run_differential.cmake"
        )
        set_tests_properties("differential_${TNAME}" PROPERTIES
            SKIP_REGULAR_EXPRESSION "SKIP: JIT bu fixture")
        # BASE.jit_known_broken (opsiyonel): "wip_" LSP senaryo deseniyle
        # aynı fikir — JIT bu fixture'da tespit edilmiş, ayrı issue'da
        # kayıtlı bir çökme/parity hatası üretiyor (SKIP_REGULAR_EXPRESSION
        # yakalayamıyor çünkü "desteklenmeyen opcode" değil gerçek bir
        # crash). VM (normatif backend, bu dosyanın kendi ölçtüğü davranış)
        # doğru; JIT [EXPERIMENTAL] ve parity bu release kapısının şartı
        # değil (AGENTS.md §9). Test kırmızı KALIR (WILL_FAIL onu yeşile
        # çevirmez, yalnız "beklenen kırmızı" olarak işaretler) — JIT
        # düzeltilip fixture gerçekten geçtiğinde WILL_FAIL kaldırılır ve
        # bu, "beklenmedik şekilde geçti" diye görünür hale gelir.
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.jit_known_broken")
            set_tests_properties("differential_${TNAME}" PROPERTIES WILL_FAIL TRUE)
        endif()
    endif()

    # #156: BASE.expected_exit (opsiyonel) — mevcut varsa run_golden_error.cmake
    # yalnız "exit != 0" yerine merkezi 0/64/65/70 sınıfının exact değerini
    # doğrular. Dosya yoksa davranış değişmez (geriye dönük uyumlu).
    set(EXPECTED_EXIT_FILE "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.expected_exit")
    set(EXPECTED_EXIT_ARG "")
    if(EXISTS "${EXPECTED_EXIT_FILE}")
        file(STRINGS "${EXPECTED_EXIT_FILE}" EXPECTED_EXIT_VALUE LIMIT_COUNT 1)
        set(EXPECTED_EXIT_ARG "-DEXPECTED_EXIT=${EXPECTED_EXIT_VALUE}")
    endif()

    # Negatif test: .compile_error — derleme hatası beklenen golden testler.
    # Dosya içeriği stderr'de aranır (regex/düz dize).
    set(COMPILE_ERROR_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.compile_error")
    if(EXISTS "${COMPILE_ERROR_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}_compile_error"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${COMPILE_ERROR_EXPECTED}
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                ${EXPECTED_EXIT_ARG}
                -P "${CMAKE_SOURCE_DIR}/cmake/run_golden_error.cmake"
        )
    endif()

    # Negatif test: .runtime_error — çalışma zamanı hatası beklenen golden testler.
    # Aynı altyapı: exit code != 0 (veya BASE.expected_exit varsa exact değer)
    # + stderr içeriği kontrolü.
    set(RUNTIME_ERROR_EXPECTED "${CMAKE_SOURCE_DIR}/tests/golden/${BASE}.runtime_error")
    if(EXISTS "${RUNTIME_ERROR_EXPECTED}")
        add_test(
            NAME "golden_${TNAME}_runtime_error"
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=${SAQUT_BINARY}
                -DSOURCE=${SQT_FILE}
                -DEXPECTED=${RUNTIME_ERROR_EXPECTED}
                -DEXTRA_ARGS=${EXTRA_ARGS_JOINED}
                ${EXPECTED_EXIT_ARG}
                -P "${CMAKE_SOURCE_DIR}/cmake/run_golden_error.cmake"
        )
    endif()
endforeach()

# LSP golden testleri — tests/lsp/*.jsonl senaryolarını otomatik keşfet.
# Her senaryonun yanında aynı adla BASE.expected.jsonl bulunmalı.
# "wip_" önekli senaryolar bilinen-bozuk davranışı belgeler: beklenen dosya
# DOĞRU/gelecekteki davranışı tanımlar, bugünkü çıktı onunla uyuşmaz —
# bu yüzden WILL_FAIL ile işaretlenir. İlgili faz düzeltince test
# "beklenmedik şekilde geçti" diye görünür; bu, WILL_FAIL'i kaldırıp
# dosyayı wip_ önekinden kurtarma zamanı geldiğinin sinyalidir.
find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE)
    file(GLOB ALL_LSP_SCENARIOS CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tests/lsp/*.jsonl"
    )
    foreach(SCEN_FILE ${ALL_LSP_SCENARIOS})
        get_filename_component(SCEN_NAME "${SCEN_FILE}" NAME)
        if(SCEN_NAME MATCHES "\\.expected\\.jsonl$")
            continue()
        endif()
        string(REGEX REPLACE "\\.jsonl$" "" SCEN_BASE "${SCEN_NAME}")
        set(SCEN_EXPECTED "${CMAKE_SOURCE_DIR}/tests/lsp/${SCEN_BASE}.expected.jsonl")
        if(NOT EXISTS "${SCEN_EXPECTED}")
            continue()
        endif()

        add_test(
            NAME "lsp_${SCEN_BASE}"
            COMMAND "${PYTHON3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tests/lsp/lsp_test_driver.py"
                --binary "${SAQUT_BINARY}"
                --fixtures "${CMAKE_SOURCE_DIR}/tests/lsp/fixtures"
                --scenario "${SCEN_FILE}"
                --expected "${SCEN_EXPECTED}"
        )

        if(SCEN_BASE MATCHES "^wip_")
            set_tests_properties("lsp_${SCEN_BASE}" PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
# DAP golden testleri — tests/dap/*.jsonl senaryolarını otomatik keşfet.
# Format LSP testleriyle aynı. "wip_" önekli senaryolar WILL_FAIL.
file(GLOB ALL_DAP_SCENARIOS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/dap/*.jsonl"
)
foreach(SCEN_FILE ${ALL_DAP_SCENARIOS})
    get_filename_component(SCEN_NAME "${SCEN_FILE}" NAME)
    if(SCEN_NAME MATCHES "\\.expected\\.jsonl$")
        continue()
    endif()
    string(REGEX REPLACE "\\.jsonl$" "" SCEN_BASE "${SCEN_NAME}")
    set(SCEN_EXPECTED "${CMAKE_SOURCE_DIR}/tests/dap/${SCEN_BASE}.expected.jsonl")
    if(NOT EXISTS "${SCEN_EXPECTED}")
        continue()
    endif()

    add_test(
        NAME "dap_${SCEN_BASE}"
        COMMAND "${PYTHON3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/dap/dap_test_driver.py"
            --binary "${SAQUT_BINARY}"
            --fixtures "${CMAKE_SOURCE_DIR}/tests/dap/fixtures"
            --scenario "${SCEN_FILE}"
            --expected "${SCEN_EXPECTED}"
    )

    if(SCEN_BASE MATCHES "^wip_")
        set_tests_properties("dap_${SCEN_BASE}" PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

else()
    message(WARNING "python3 bulunamadı — LSP golden testleri (tests/lsp/) atlanıyor")
endif()
