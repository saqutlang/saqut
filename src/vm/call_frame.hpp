// ============================================================================
// saQut VM — CallFrame (Tek Fonksiyon Çağrısının Çalışma Alanı)
//
// fibonacci(5) çağrıldığında bir CallFrame açılır.
// fibonacci(4) çağrıldığında AYRI bir CallFrame daha açılır.
// Her frame kendi slot dizisine sahiptir — üst frame'e asla dokunmaz.
//
// FRAME YAŞAM DÖNGÜSÜ:
//   1. CALL instruction'ı çalışır → yeni CallFrame oluşturulur, callStack'e eklenir
//   2. Interpreter bu frame'in instruction'larını çalıştırır
//   3. RETURN instruction'ı çalışır → frame callStack'ten çıkarılır,
//      dönüş değeri caller'ın `returnDestSlot`'una yazılır
//
// REFERANS GÜVENLİĞİ:
//   Interpreter döngüsü her iterasyonda callStack.back() ile frame'i TAZELER.
//   CALL ve RETURN'den sonra `continue` ile döngü başına dönülür.
//   Bu sayede vector büyüyüp referansı geçersiz kılsa bile sorun olmaz.
// ============================================================================

#ifndef SAQUT_VM_CALL_FRAME
#define SAQUT_VM_CALL_FRAME

#include <vector>
#include "ir/ir_function.hpp"
#include "vm/value.hpp"

struct CallFrame {
    // Hangi fonksiyonun instruction'larını çalıştırıyoruz?
    // Pointer — IRProgram sahibi, frame sahibi değil.
    const IRFunction* function = nullptr;

    // Sıradaki çalıştırılacak instruction'ın indeksi.
    // Döngü her adımda önce bu indeksteki instruction'ı alır,
    // SONRA ip'yi artırır. CALL/RETURN ip'ye dokunmaz.
    int instructionPointer = 0;

    // Hata ayıklama: bu frame'de en son çalışan komutun satırı. Breakpoint
    // yalnız satıra GİRİŞTE tetiklenir; çağrıdan aynı satıra dönüşte
    // (`y = add(x, 2);`) yeniden tetiklenmez.
    int lastLine = 0;

    // Bu frame'in değer depoları: parametreler + lokaller + geçiciler.
    // Boyut = function->slotCount (frame oluşturulurken ayarlanır).
    std::vector<Value> slots;

    // RETURN olunca dönüş değeri CALLER'ın hangi slotuna yazılacak?
    // -1 = main fonksiyonu (caller yok, değer kullanılmaz).
    int returnDestSlot = -1;
};

#endif // SAQUT_VM_CALL_FRAME
