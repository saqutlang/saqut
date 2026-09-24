// ============================================================================
// saQut — Thread'ler arası mesaj serileştirme (ADR-045 Faz 2-c)
//
// DİZİN:   src/runtime/threading/message.hpp
// KATMAN:  runtime/threading — isolate sınırını geçen değerler
//
// ADR-045 §MESAJ SEMANTİĞİ 10: struct/array/string DEEP COPY ile geçer
// (graph kopyası; içteki aliasing ve döngüler korunur). Mesaj heap DIŞINDA
// yaşar ve heap pointer'ı İÇERMEZ: gönderenin heap'inden tamamen bağımsız
// bir bayt dizisi + değişmez struct metadata'sına (alan adları) paylaşımlı
// referanslar. Alıcı deserialize ile KENDİ heap'inde yeni nesneler kurar.
//
// PLAN B (docs/threading-decisions.md [2-c]): serileştirme TİP YÖNLÜ değil
// YAPISALDIR — Value::kind ve nesne başlığındaki tür (Array + elemKind /
// Struct / String / Decimal) yeterlidir; çalışma zamanında tip tablosuna
// gerek yoktur. Gönderilebilirlik (ör. native handle) derleme zamanında
// denetlenir (Faz 3).
//
// Biçim (küçük uçlu, hizasız):
//   değer  := kind:u8 yük
//   Int/LongInt/Date: i64   Float/Float32: f64   Decimal: coeff:i64 exp:i32
//   Null: —   String/Ref: nesne
//   nesne  := 0:u8 geriref:u32  |  1:u8 tür:u8 içerik   (ilk görüşte indeks alır)
//   String: len:u64 bayt   Decimal: coeff:i64 exp:i32
//   Array:  elemKind:u8 n:u64 (Ref: n×değer | paketli: ham elemanlar)
//   Struct: n:u32 meta:u32 (0xFFFFFFFF = yok) n×değer
// ============================================================================

#ifndef SAQUT_RUNTIME_THREADING_MESSAGE
#define SAQUT_RUNTIME_THREADING_MESSAGE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vm/value.hpp"

struct Heap;

namespace saqut::threading {

struct MessageBuffer {
    std::vector<uint8_t> bytes;
    // Struct alan adı metadata'sı: değişmez, ref-sayımlı (atomik) — heap
    // nesnesi değildir, isolate'ler arası paylaşılabilir.
    std::vector<std::shared_ptr<std::vector<std::string>>> metas;

    void clear() { bytes.clear(); metas.clear(); }
    bool empty() const { return bytes.empty(); }
};

// v'yi (ve eriştiği tüm nesne grafını) buf'un sonuna ekler.
void serialize(const Value& v, MessageBuffer& buf);

// buf'tan (baştan) bir değer okur; nesneler heap'te yeni tahsis edilir.
// buf DEĞİŞMEZ — aynı mesaj birden çok thread'den eşzamanlı okunabilir
// (List::get kilitsizdir). Tahsisler toplama tetiklemez (Heap sözleşmesi) —
// dönen değer çağıranın köküne bağlanana kadar güvendedir.
Value deserialize(const MessageBuffer& buf, Heap& heap);

// Kolaylık: tek değerlik mesaj.
inline MessageBuffer makeMessage(const Value& v) {
    MessageBuffer m;
    serialize(v, m);
    return m;
}

}  // namespace saqut::threading

#endif  // SAQUT_RUNTIME_THREADING_MESSAGE
