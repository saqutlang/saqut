// ============================================================================
// saQut — Veri Tipi Registry'si
// ============================================================================
//
// DİZİN:   src/data/data_registry.hpp
// KATMAN:  data — tüm veri tipi modüllerini tek indeks uzayında toplar
//
// AMAÇ (#223):
//   string.cpp / array.cpp / struct.cpp kendi metod tablolarını verir; bu
//   registry onları tek bir düz listede birleştirir ve iki tüketiciye sunar:
//
//     1. Derleme zamanı  — TypeChecker, SymbolTable, LSP: imza doğrulama,
//                          dönüş tipi çözme, otomatik tamamlama.
//     2. Çalışma zamanı  — rt_host_call: id → thunk, O(1).
//
//   İkisi AYNI kaydı okur. Eskiden imza builtin_methods.hpp'de, gövde
//   interpreter.cpp'deki switch'teydi ve aralarında elle korunan bir sıra
//   sözleşmesi vardı — bir metodu tabloda yukarı taşımak sessizce yanlış
//   gövdeyi çağırırdı. Artık ayrılamazlar.
//
// YENİ METOT EKLEMEK: ilgili modülün tablosunun sonuna bir satır.
//
// METODU OLAN YENİ BİR ALICI TİPİ EKLEMEK:
//   1. src/data/<tip>.{hpp,cpp} yaz, data<Tip>Methods() sağla
//   2. DataMethodCategory'ye bir değer, dataReceiverCategory()'ye bir dal
//   3. dataAllMethods() içine bir satır ekle
//   DataMethodCategory üzerindeki switch'ler (TypeChecker) eksik dalı
//   derleyici uyarısıyla (-Wswitch) gösterir.
//
// ============================================================================

#ifndef SAQUT_DATA_REGISTRY
#define SAQUT_DATA_REGISTRY

#include <optional>
#include <string>
#include <vector>

#include "data/data_type.hpp"

// Tüm veri tiplerinin metodları, tek düz liste. İndeks = runtime id.
//
// Sıra, modüllerin dataAllMethods() içindeki sırasıdır. id'ler IR'ye gömülür
// (CALLHOST::intValue); ADR-044 gereği kararlı tutulur: yeni kayıtlar SONA
// eklenir.
const std::vector<DataMethod>& dataAllMethods();

// ── Derleme zamanı arama — TypeChecker ve LSP aynı fonksiyonları kullanır ──

// Alıcı tipinin metot kategorisi: E[] → Array, string → StringVal,
// struct → StructVal. Metodu olmayan tipler (int, bool, date, ...) için
// nullopt. Bir tipin hangi metot ailesine gittiği YALNIZ burada karar verilir.
std::optional<DataMethodCategory> dataReceiverCategory(const Type& receiver);

// Kategoride `methodName` adlı metot; yoksa nullptr.
const DataMethod* dataFindMethod(DataMethodCategory category, const std::string& methodName);

// Metot bu alıcıda geçerli mi? params[0] sabit bir tipse (toString → byte[])
// alıcı o tip olmalıdır; kategori kuralıyla verilmişse her alıcı geçerlidir.
bool dataMethodAcceptsReceiver(const DataMethod& method, const Type& receiver);

// Tanı ipuçları için: kategorinin bu alıcıda geçerli metot adları,
// tablo sırasıyla ("length, push, pop, ...").
std::string dataMethodNames(DataMethodCategory category, const Type& receiver);

// Bir metodun runtime id'si (dataAllMethods indeksi). Bulunamazsa -1.
int dataMethodId(const DataMethod* m);

// id → kayıt; geçersizse nullptr.
const DataMethod* dataMethodAt(int id);

// TypeChecker yardımcısı: leftName'den eleman tipini çöz.
// "int" → int, "string" → string, "Person" → struct Person
Type dataResolveElemType(const std::string& leftName);

#endif // SAQUT_DATA_REGISTRY
