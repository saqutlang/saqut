// ============================================================================
// saQut FFI — Host Fonksiyon Registry (Sayısal Dispatch)
// ============================================================================
//
// DİZİN:   src/ffi/host_functions.hpp
// KATMAN:  FFI — gömülü (C++ gövdeli) host fonksiyonlarının tek kaynağı
//
// AMAÇ (ADR-034, #107):
//   Gömülü root.sqt'teki `ffi ... : HOST_ID from mod` bildirimleri sembolik
//   HOST_ID taşır. Bu registry HOST_ID → sayısal index eşlemesini (derleme
//   zamanı) ve index → C++ gövde dispatch'ini (runtime, O(1)) sağlar.
//
//   Sayılar TABLONUN SIRASINDAN gelir; root.sqt ham sayı yazmaz, sembolik ad
//   kullanır → hostEntryIndex ile çözülür (#229; kHostFnBase toplamı orada).
//   Ad tabloda yoksa (drift) symbol collector hata verir.
//
// ============================================================================

#ifndef SAQUT_FFI_HOST_FUNCTIONS
#define SAQUT_FFI_HOST_FUNCTIONS

#include <set>
#include <string>
#include <vector>
#include "vm/value.hpp"
#include "gc/gc_object.hpp"
#include "gc/gc_heap.hpp"
#include "ffi/host_abi.hpp"

// Tek bir host fonksiyon kaydı — HostEntry'nin (host_abi.hpp) alias'ı.
//
// #229 + AGENTS.md §10.2: alan-kopyası struct yerine alias — HostEntry zaten
// tam kaydı taşır (symbolicId, arity int8_t, flags, retKind, thunk); ayrı bir
// HostFn struct'ı ikinci tanım olurdu. Date fonksiyonları (15) ayrı tablodadır
// ve aynı tamlıktadır (src/data/date.cpp).
using HostFn = HostEntry;

// Alt küme tabloları (bölüm başına ayrı TU; src/ffi/functions/{math,fs,sys,date,core,
// process,io,path,utf8,os,net}.cpp). Her biri kendi sabit tablosunu kurar (tek tanım,
// §10.2). io.cpp stdin/stdout/stderr modüllerini birden barındırır; os.cpp
// os + terminal modüllerini; net.cpp net + tls modüllerini.
const std::vector<HostFn>& mathHostFunctions();
const std::vector<HostFn>& fsHostFunctions();
const std::vector<HostFn>& sysHostFunctions();
const std::vector<HostFn>& dateHostFunctions();
const std::vector<HostFn>& coreHostFunctions();
const std::vector<HostFn>& processHostFunctions();
const std::vector<HostFn>& stdinHostFunctions();
const std::vector<HostFn>& stdoutHostFunctions();
const std::vector<HostFn>& stderrHostFunctions();
const std::vector<HostFn>& pathHostFunctions();
const std::vector<HostFn>& utf8HostFunctions();
const std::vector<HostFn>& osHostFunctions();
const std::vector<HostFn>& terminalHostFunctions();
// net.cpp — net + tls modülleri (motor: src/net/net_runtime.cpp).
const std::vector<HostFn>& netHostFunctions();
const std::vector<HostFn>& tlsHostFunctions();

// Tüm gömülü host fonksiyonların düz tablosu (math/fs/sys/date_now/core/...
// yeni modüller). Index = sayısal host id; root.sqt'e gömülmez, hostEntryIndex
// ile çözülür. Alt küme tablolarını tek listede birleştirir (host_functions.cpp).
const std::vector<HostFn>& hostFnTable();

#endif // SAQUT_FFI_HOST_FUNCTIONS
