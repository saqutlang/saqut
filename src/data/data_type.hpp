// ============================================================================
// saQut — Veri Tipi Modülü Sözleşmesi
// ============================================================================
//
// DİZİN:   src/data/data_type.hpp
// KATMAN:  data — çalışma zamanı veri tiplerinin TEK sahibi
//
// AMAÇ (#223):
//   Bir veri tipinin (string, array, struct, ...) çalışma zamanındaki her şeyi
//   TEK dosyada toplanır: bellek temsili, metod imzaları, metod gövdeleri ve
//   backend'lerin çağırdığı arayüz.
//
//   "String'de bir şey değiştirmek istiyorum" → src/data/string.* açılır.
//   Başka hiçbir yere dokunulmaz.
//
// ÖNCESİNDE NE VARDI (ölçüldü):
//   string mantığı 13 dosyaya yayılmıştı — interpreter.cpp (33 dokunuş),
//   mir_backend.cpp (27), builtin_methods.hpp (20), object.hpp, value.hpp...
//   Metod İMZALARI builtin_methods.hpp'de, GÖVDELERİ interpreter.cpp'deki
//   398 satırlık switch'teydi ve aralarında ELLE korunan bir runtimeId sıra
//   sözleşmesi vardı (yorumla belgelenmiş: "length(0), push(1), pop(2)...").
//   init() içinde bir metodu yukarı taşımak sessizce yanlış gövdeyi çağırırdı.
//
//   Bu dosya o sözleşmeyi ortadan kaldırır: id, imza ve gövde AYNI kayıtta
//   durur. Ayrılamazlar, dolayısıyla kayamazlar.
//
// KAPSAM SINIRI:
//   Buraya giren: bellek temsili, metodlar, backend arayüzü.
//   Buraya GİRMEYEN: GC işaretleme/süpürme (vm/object.*), IR üretimi,
//   tip denetimi. Onlar tipe değil derleyici aşamasına aittir.
//
// ============================================================================

#ifndef SAQUT_DATA_TYPE
#define SAQUT_DATA_TYPE

#include <string>
#include <vector>

#include "core/type.hpp"
#include "ffi/host_abi.hpp"

// ----------------------------------------------------------------------------
// Metod kategorisi — receiver'ın nasıl çözüleceğini belirler.
// ----------------------------------------------------------------------------
enum class DataMethodCategory {
    Array,      // E[] receiver — herhangi bir eleman tipi için
    StringVal,  // string receiver
    StructVal,  // S receiver (tanımlı struct tipleri)
};

// ----------------------------------------------------------------------------
// Parametre ve dönüş tipi kuralları.
//
// Bir metodun imzası receiver'a bağlı olabilir: E::push(E[], E) — ikinci
// argümanın tipi çağrının sol tarafından türetilir. Bu yüzden sabit Type
// yetmez, kural gerekir.
// ----------------------------------------------------------------------------
enum class DataParamKind {
    Fixed,      // sabit tip (int indeks, string ayraç)
    ElemType,   // E — receiver'dan türetilir
    ElemArray,  // E[] — receiver'dan türetilir
    StringVal,  // string (StringVal metodlarının receiver'ı)
};

struct DataParamRule {
    DataParamKind kind;
    Type          fixedType;  // kind == Fixed ise
};

enum class DataReturnKind {
    Fixed,      // sabit tip
    ElemType,   // E döner (pop, remove)
    ElemArray,  // E[] döner (slice, concat, reverse)
};

struct DataReturnRule {
    DataReturnKind kind;
    Type           fixedType;  // kind == Fixed ise
};

inline DataParamRule dpFixed(Type t) { return { DataParamKind::Fixed, std::move(t) }; }
inline DataParamRule dpElem()        { return { DataParamKind::ElemType,  {} }; }
inline DataParamRule dpElemArray()   { return { DataParamKind::ElemArray, {} }; }
inline DataParamRule dpString()      { return { DataParamKind::StringVal, {} }; }

inline DataReturnRule drFixed(Type t) { return { DataReturnKind::Fixed, std::move(t) }; }
inline DataReturnRule drElem()        { return { DataReturnKind::ElemType,  {} }; }
inline DataReturnRule drElemArray()   { return { DataReturnKind::ElemArray, {} }; }

// ----------------------------------------------------------------------------
// DataMethod — bir metodun TAMAMI: imza + gövde, tek kayıtta.
//
// Kritik tasarım noktası: `thunk` burada, imzanın yanında durur. Eskiden imza
// bir dosyada, gövde başka dosyadaki bir switch case'indeydi ve ikisini
// birbirine bağlayan tek şey elle korunan bir sayı sırasıydı. Artık
// ayrılamazlar.
//
// thunk imzası host ABI'siyle AYNIDIR (host_abi.hpp): VM ve her backend
// built-in metodları host fonksiyonlarıyla tamamen aynı yoldan çağırır.
// Ayrı bir dispatch mekanizması yoktur.
//
// Alıcıyı yerinde değiştirme: yalnız heap nesnesi alıcılarda (array, struct)
// mümkündür — thunk nesnenin kendisini değiştirir. Değer tipli bir alıcıyı
// (string, sayı) değiştiren metot tanımlanamaz: thunk alıcının kopyasını alır
// ve IR sonucu değişkene geri yazmaz. `flags` içindeki HOST_MUTATING yalnız
// belgeleme niteliğindedir.
//
// Alıcı kısıtı: params[0] kategori için genel kuraldır (dpElemArray: her E[],
// dpString: string, dpElem: struct). Metot yalnız belirli bir alıcı tipinde
// geçerliyse params[0] o tipi sabit verir — ör. toString: dpFixed(byte[]).
// TypeChecker ve LSP bunu dataMethodAcceptsReceiver() ile okur.
// ----------------------------------------------------------------------------
struct DataMethod {
    const char*                name;      // "replace", "push", "toJson"
    DataMethodCategory         category;
    // params[0] = receiver, params[1..] = diğer argümanlar
    std::vector<DataParamRule> params;
    DataReturnRule             ret;
    HostKind                   retKind;   // sınır temsilindeki dönüş türü
    uint8_t                    flags;     // HOST_* (heap gerekir mi, vb.)
    HostThunk                  thunk;     // GÖVDE — imzayla aynı yerde
};

// ----------------------------------------------------------------------------
// Her veri tipi modülü bu fonksiyonu sağlar.
//
// string.cpp → dataStringMethods()
// array.cpp  → dataArrayMethods()
// struct.cpp → dataStructMethods()
//
// registry.cpp bunları toplar ve tek indeks uzayına yerleştirir.
// ----------------------------------------------------------------------------
const std::vector<DataMethod>& dataStringMethods();
const std::vector<DataMethod>& dataArrayMethods();
const std::vector<DataMethod>& dataStructMethods();

#endif // SAQUT_DATA_TYPE
