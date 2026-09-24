# run_golden.cmake — tek bir golden test çalıştırır ve çıktıyı karşılaştırır.
#
# Parametreler (cmake -D ile geçilir):
#   BINARY    — saqut binary yolu
#   SOURCE    — test .sqt dosyası (tam yol)
#   EXPECTED  — beklenen çıktı dosyası (tam yol)
#   COMMAND   — "run" (varsayılan) veya "ir"
#   OPTIMIZED — 1 ise --optimized bayrağı eklenir

if(NOT DEFINED COMMAND)
    set(COMMAND "run")
endif()

set(EXTRA_FLAGS "")
if(OPTIMIZED)
    list(APPEND EXTRA_FLAGS "--optimized")
endif()
# Optimizasyon varsayılan açık (src/cli/args.hpp); plain IR golden'ları
# açıkça kapatmalı.
if(DONT_OPTIMIZE)
    list(APPEND EXTRA_FLAGS "--dont-optimize")
endif()
# ADR-036 (#76): BASE.flags'ten gelen --allow-fs vb. bayraklar ("|" ile ayrık).
if(EXTRA_ARGS)
    string(REPLACE "|" ";" EXTRA_ARGS_LIST "${EXTRA_ARGS}")
    list(APPEND EXTRA_FLAGS ${EXTRA_ARGS_LIST})
endif()

execute_process(
    COMMAND "${BINARY}" "${COMMAND}" ${EXTRA_FLAGS} "file:${SOURCE}"
    OUTPUT_VARIABLE ACTUAL
    ERROR_VARIABLE  STDERR_OUT
    RESULT_VARIABLE EXIT_CODE
)

if(NOT EXIT_CODE EQUAL 0)
    message(FATAL_ERROR
        "saqut ${COMMAND} başarısız (exit ${EXIT_CODE}):\n${STDERR_OUT}")
endif()

file(READ "${EXPECTED}" EXPECTED_CONTENT)

# ANSI renk kodlarını temizle (terminal dışı karşılaştırma için)
string(ASCII 27 ESC)
string(REGEX REPLACE "${ESC}\\[[0-9;]*m" "" ACTUAL "${ACTUAL}")
string(REGEX REPLACE "${ESC}\\[1m" "" ACTUAL "${ACTUAL}")

if(NOT ACTUAL STREQUAL EXPECTED_CONTENT)
    message(FATAL_ERROR
        "Çıktı uyuşmuyor: ${SOURCE}\n"
        "--- BEKLENEN ---\n${EXPECTED_CONTENT}"
        "--- GERÇEK ---\n${ACTUAL}"
    )
endif()
