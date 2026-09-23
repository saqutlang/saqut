# run_differential.cmake — VM ile MIR JIT'i aynı fixture üzerinde koşup
# stdout + exit code'u bayt-bayt karşılaştırır (#92).
#
# JIT henüz MIR dilimlerinin desteklemediği bir opcode gördüğünde programın
# TAMAMINI reddeder (mir_backend.hpp: kısmi JIT yok) — bu, bir parity hatası
# DEĞİL, henüz kapsanmamış bir dilim demektir. Böyle durumda test "SKIP:"
# damgasıyla biter ve CMakeLists.txt'teki SKIP_REGULAR_EXPRESSION ile ctest
# tarafından "Not Run" (atlandı) olarak işaretlenir — pass/fail sayısına
# karışmaz. JIT programı TAM derleyip çalıştırdığında (kısmi değil) VM'in
# çıktısından farklı bir şey üretirse bu GERÇEK bir VM≡JIT parity hatasıdır
# ve test FATAL_ERROR ile kırmızı yanar.
#
# Parametreler (cmake -D ile geçilir):
#   BINARY    — saqut binary yolu
#   SOURCE    — test .sqt dosyası (tam yol)
#   EXTRA_ARGS — BASE.flags'ten "|" ile ayrık ekstra bayraklar (opsiyonel)

set(EXTRA_FLAGS "")
if(EXTRA_ARGS)
    string(REPLACE "|" ";" EXTRA_FLAGS "${EXTRA_ARGS}")
endif()

execute_process(
    COMMAND "${BINARY}" run ${EXTRA_FLAGS} "file:${SOURCE}"
    OUTPUT_VARIABLE VM_OUT
    ERROR_VARIABLE  VM_ERR
    RESULT_VARIABLE VM_EXIT
)

execute_process(
    COMMAND "${BINARY}" run --jit ${EXTRA_FLAGS} "file:${SOURCE}"
    OUTPUT_VARIABLE JIT_OUT
    ERROR_VARIABLE  JIT_ERR
    RESULT_VARIABLE JIT_EXIT
)

if(JIT_ERR MATCHES "unsupported opcode")
    message(STATUS "SKIP: JIT bu fixture'i henuz derlemiyor — ${SOURCE}\n${JIT_ERR}")
    return()
endif()

if(NOT VM_OUT STREQUAL JIT_OUT OR NOT VM_EXIT EQUAL JIT_EXIT)
    message(FATAL_ERROR
        "VM=/=JIT parity hatasi: ${SOURCE}\n"
        "--- VM  (exit ${VM_EXIT}) ---\n${VM_OUT}\n"
        "--- JIT (exit ${JIT_EXIT}) ---\n${JIT_OUT}\n"
        "--- JIT stderr ---\n${JIT_ERR}"
    )
endif()
