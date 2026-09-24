# run_golden_error.cmake — derleme hatası BEKLEYEN golden test.
#
# Parametreler (cmake -D ile geçilir):
#   BINARY        — saqut binary yolu
#   SOURCE        — test .sqt dosyası (tam yol)
#   EXPECTED      — beklenen hata içeriği (.compile_error dosyası); stderr bu
#                   metni içermeli (regex veya düz dize olarak eşleşir)
#   EXPECTED_EXIT — opsiyonel (#156). Verilirse exit kodu tam bu değere eşit
#                   olmalı (merkezi 0/64/65/70 sınıfı); verilmezse eski
#                   davranış korunur: yalnız "exit != 0" kontrol edilir.

# ADR-036 (#76): BASE.flags'ten gelen --allow-fs vb. bayraklar ("|" ile ayrık).
set(EXTRA_FLAGS "")
if(EXTRA_ARGS)
    string(REPLACE "|" ";" EXTRA_FLAGS "${EXTRA_ARGS}")
endif()

execute_process(
    COMMAND "${BINARY}" run ${EXTRA_FLAGS} "file:${SOURCE}"
    OUTPUT_VARIABLE STDOUT_OUT
    ERROR_VARIABLE  STDERR_OUT
    RESULT_VARIABLE EXIT_CODE
)

if(DEFINED EXPECTED_EXIT AND NOT EXPECTED_EXIT STREQUAL "")
    if(NOT EXIT_CODE EQUAL EXPECTED_EXIT)
        message(FATAL_ERROR
            "Beklenen exact exit ${EXPECTED_EXIT}, gerçek ${EXIT_CODE}: ${SOURCE}\n"
            "Çıktı: ${STDOUT_OUT}\nStderr: ${STDERR_OUT}")
    endif()
elseif(EXIT_CODE EQUAL 0)
    message(FATAL_ERROR
        "Derleme hatası bekleniyordu ama program başarıyla çalıştı: ${SOURCE}\n"
        "Çıktı: ${STDOUT_OUT}")
endif()

# Opsiyonel: BASE.expected aynı fixture'da varsa (runtime hatasından ÖNCE
# üretilen stdout, ör. "hata satırından sonrası çalışmamalı" testleri) stdout
# tam olarak ona eşit olmalı. tests/run.sh ile aynı kural.
if(DEFINED EXPECTED_STDOUT AND NOT EXPECTED_STDOUT STREQUAL "")
    file(READ "${EXPECTED_STDOUT}" EXPECTED_STDOUT_CONTENT)
    string(STRIP "${EXPECTED_STDOUT_CONTENT}" EXPECTED_STDOUT_CONTENT)
    string(STRIP "${STDOUT_OUT}" STDOUT_STRIPPED)
    if(NOT STDOUT_STRIPPED STREQUAL EXPECTED_STDOUT_CONTENT)
        message(FATAL_ERROR
            "stdout uyuşmuyor: ${SOURCE}\n"
            "--- BEKLENEN ---\n${EXPECTED_STDOUT_CONTENT}\n--- GERÇEK ---\n${STDOUT_OUT}")
    endif()
endif()

file(READ "${EXPECTED}" EXPECTED_CONTENT)
string(STRIP "${EXPECTED_CONTENT}" EXPECTED_CONTENT)

if(NOT STDERR_OUT MATCHES "${EXPECTED_CONTENT}")
    message(FATAL_ERROR
        "Beklenen '${EXPECTED_CONTENT}' mesajı stderr'de bulunamadı: ${SOURCE}\n"
        "Stderr: ${STDERR_OUT}")
endif()
