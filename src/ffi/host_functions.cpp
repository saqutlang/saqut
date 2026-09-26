// ============================================================================
// saQut FFI — Host Fonksiyon Registry Birleştiricisi
// ============================================================================
//
// Gömülü (C++ gövdeli) host fonksiyonlarının tek düz dispatch tablosu.
// Gövdeler ilgi alanına göre src/ffi/functions/ altında ayrı TU'lardadır:
//   functions/{math, fs, sys, date, core, process, io, path, utf8, os, net}.cpp
// Bu dosya o alt tabloları tek hostFnTable() listesinde birleştirir.
//
// Alt tablo sırası sayısal host id'yi belirler; root.sqt sembolik HOST_ID
// kullandığı için gövdelerin sırası değişebilir (#229, #115 organizasyonu).
// ============================================================================

#include "ffi/host_functions.hpp"

const std::vector<HostFn>& hostFnTable() {
    static const std::vector<HostFn> table = [] {
        std::vector<HostFn> all;
        const auto& mathRef = mathHostFunctions(); all.insert(all.end(), mathRef.begin(), mathRef.end());
        const auto& fsRef   = fsHostFunctions();   all.insert(all.end(), fsRef.begin(), fsRef.end());
        const auto& sysRef  = sysHostFunctions();  all.insert(all.end(), sysRef.begin(), sysRef.end());
        const auto& dateRef = dateHostFunctions(); all.insert(all.end(), dateRef.begin(), dateRef.end());
        const auto& coreRef = coreHostFunctions(); all.insert(all.end(), coreRef.begin(), coreRef.end());
        const auto& procRef = processHostFunctions(); all.insert(all.end(), procRef.begin(), procRef.end());
        // io.cpp — stdin/stdout/stderr üç ayrı yüzey modülü, tek TU.
        const auto& inRef   = stdinHostFunctions();   all.insert(all.end(), inRef.begin(), inRef.end());
        const auto& outRef  = stdoutHostFunctions();  all.insert(all.end(), outRef.begin(), outRef.end());
        const auto& errRef  = stderrHostFunctions();  all.insert(all.end(), errRef.begin(), errRef.end());
        const auto& pathRef = pathHostFunctions(); all.insert(all.end(), pathRef.begin(), pathRef.end());
        const auto& utf8Ref = utf8HostFunctions(); all.insert(all.end(), utf8Ref.begin(), utf8Ref.end());
        const auto& osRef   = osHostFunctions();   all.insert(all.end(), osRef.begin(), osRef.end());
        const auto& termRef = terminalHostFunctions(); all.insert(all.end(), termRef.begin(), termRef.end());
        const auto& netRef  = netHostFunctions();  all.insert(all.end(), netRef.begin(), netRef.end());
        const auto& tlsRef  = tlsHostFunctions();  all.insert(all.end(), tlsRef.begin(), tlsRef.end());
        return all;
    }();
    return table;
}
