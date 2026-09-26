// ============================================================================
// #222: Host ABI sınır temsili — HostSlot / Value round-trip sözleşmesi
//
// Hata SINIFI: sınır temsili sessizce bilgi kaybederse (kind düşmesi, precision
// kaybı, string kopyalanmaması) host çağrıları VM'de doğru, JIT'te yanlış
// cevap verir — ve bu ancak çalışma zamanında fark edilir. Bu test dönüşümü
// tip tip sabitler.
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "ffi/host_abi.hpp"
#include "ffi/host_bridge.hpp"
#include "ffi/host_registry.hpp"
#include <set>
#include "data/data_registry.hpp"

int main() {
    // 1) Boyut ve kopyalanabilirlik sözleşmesi.
    //    JIT argümanları stack'te sabit offset'le inşa eder; bu iki özellik
    //    olmadan bu imkânsızdır.
    static_assert(sizeof(HostSlot) == 16, "HostSlot 16 bayt olmali");
    static_assert(std::is_trivially_copyable<HostSlot>::value,
                  "HostSlot trivially copyable olmali — JIT stack'te insa eder");
    // Value ile karşılaştırma: neden ayrı bir tip gerektiğinin kanıtı.
    assert(sizeof(Value) > sizeof(HostSlot));

    HostCallScratch scratch;

    // 2) Skaler round-trip — kind ve değer korunmalı.
    {
        Value v = Value::fromInt(-42);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::Int && b.intValue() == -42);
    }
    {
        // ADR-040: longint tam 64-bit; sınırda daralma OLMAMALI.
        Value v = Value::fromLongInt(9223372036854775807LL);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::LongInt);
        assert(b.int64Value() == 9223372036854775807LL);
    }
    {
        Value v = Value::fromLongInt(-9223372036854775807LL - 1);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.int64Value() == -9223372036854775807LL - 1);
    }
    {
        Value v = Value::fromFloat(3.141592653589793);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::Float && b.floatValue() == 3.141592653589793);
    }
    {
        // Float32: (float) truncate ADR-040 gereği KORUNMALI — sınırdan
        // geçerken sessizce double precision'a yükselmemeli.
        Value v = Value::fromFloat32(0.1);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::Float32);
        assert(b.floatValue() == (double)(float)0.1);
    }
    {
        Value v = Value::fromDate(1700000000000LL);
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::Date && b.int64Value() == 1700000000000LL);
    }
    {
        Value v = Value::null();
        Value b = fromHostSlot(toHostSlot(v, scratch));
        assert(b.kind == ValueKind::Null);
    }

    // 3) String — inline (VM) ↔ pointer (sınır) çevrimi içeriği korumalı.
    {
        Value v = Value::fromString("merhaba dünya");
        HostSlot s = toHostSlot(v, scratch);
        assert(s.kind == HostKind::Str && s.p != nullptr);
        assert(hostAsString(s) == "merhaba dünya");
        Value b = fromHostSlot(s);
        assert(b.kind == ValueKind::String && b.stringValue() == "merhaba dünya");
    }
    {
        // Boş string null DEĞİLDİR — ayrım korunmalı.
        Value v = Value::fromString("");
        HostSlot s = toHostSlot(v, scratch);
        assert(s.kind == HostKind::Str);
        assert(!s.isNull());
        assert(fromHostSlot(s).stringValue().empty());
    }

    // 4) Decimal — kutulu geçiş değeri bozmamalı (ADR-028).
    {
        Value v = Value::fromDecimal(DecimalValue::fromString("123.456"));
        HostSlot s = toHostSlot(v, scratch);
        assert(s.kind == HostKind::Decimal && s.p != nullptr);
        Value b = fromHostSlot(s);
        assert(b.kind == ValueKind::Decimal);
        assert(b.decimalValue().toString() == "123.456");
    }

    // 5) Sayısal okuma yardımcıları — ADR-040 genişletme kuralları.
    assert(hostAsI64(HostSlot::fromInt(7)) == 7);
    assert(hostAsI64(HostSlot::fromLong(1LL << 40)) == (1LL << 40));
    assert(hostAsDouble(HostSlot::fromInt(3)) == 3.0);
    assert(hostAsDouble(HostSlot::fromFloat(2.5)) == 2.5);

    // 6) HostError — boş = başarı, dolu = hata.
    {
        HostError e;
        assert(!e.failed());
        e.set("dosya bulunamadi", "E_HOST");
        assert(e.failed() && e.code == "E_HOST");
        e.clear();
        assert(!e.failed());
    }

    // 7) Scratch yeniden kullanılabilir olmalı — çağrı başına heap tahsisi
    //    yapmamanın önkoşulu.
    {
        scratch.reset();
        assert(scratch.strings.empty() && scratch.decimals.empty());
        for (int i = 0; i < 100; ++i)
            (void)toHostSlot(Value::fromString("x"), scratch);
        assert(scratch.strings.size() == 100);
        scratch.reset();
        assert(scratch.strings.empty());
    }

    // 8) rt_host_call — TEK giriş noktası gerçekten çalışıyor mu?
    //    Adım 2'nin asıl iddiası bu: eski gövdeler yeni ABI üzerinden
    //    çağrılabiliyor ve aynı sonucu veriyor.
    {
        HostRetOwner  owner;
        HostEnv       env;
        HostCallFrame f;
        f.retOwner = &owner;
        f.env      = &env;

        // MATH_ABS(-5) → 5
        int32_t id = hostEntryIndex("MATH_ABS");
        assert(id != kHostIdInvalid);
        HostSlot a[1] = { HostSlot::fromInt(-5) };
        f.args = a; f.argc = 1;
        assert(rt_host_call(id, &f) == 0);
        assert(!f.err.failed());
        assert(fromHostSlot(f.ret).intValue() == 5);

        // MATH_SQRT(9.0) → 3.0  (float yolu)
        f.reset();
        id = hostEntryIndex("MATH_SQRT");
        HostSlot b[1] = { HostSlot::fromFloat(9.0) };
        f.args = b; f.argc = 1;
        assert(rt_host_call(id, &f) == 0);
        assert(fromHostSlot(f.ret).floatValue() == 3.0);

        // CORE_VERSION() → string dönüşü; ömür sahibi üzerinden geçmeli
        f.reset();
        id = hostEntryIndex("CORE_VERSION");
        f.args = nullptr; f.argc = 0;
        assert(rt_host_call(id, &f) == 0);
        assert(f.ret.kind == HostKind::Str);
        assert(!hostAsString(f.ret).empty());

        // Bilinmeyen sembolik ad → drift yakalanmalı (sessiz yanlış dispatch yok)
        assert(hostEntryIndex("BOYLE_BIR_SEY_YOK") == kHostIdInvalid);

        // Bağlanmamış id → açık hata, sessiz başarı DEĞİL.
        // (kBuiltinBase artık bağlı — #223; boş çekirdek bloğu kullanılır.)
        f.reset();
        f.args = nullptr; f.argc = 0;
        assert(rt_host_call(kCoreBase, &f) != 0);
        assert(f.err.failed());

        // #223: built-in metodlar da aynı giriş noktasından çalışır.
        // string::upper — id, imza ve gövde artık aynı kayıtta (DataMethod).
        {
            const DataMethod* m = dataLookupMethod("string", "upper", false, false);
            assert(m != nullptr);
            HostSlot in[1] = { HostSlot::fromStr(nullptr) };
            StringObject so("merhaba");
            in[0] = HostSlot::fromStr(&so);
            f.reset();
            f.args = in; f.argc = 1;
            assert(rt_host_call(kBuiltinBase + dataMethodId(m), &f) == 0);
            assert(hostAsString(f.ret) == "MERHABA");
        }

        // Eksik argümanla çağrı bellek hatası DEĞİL, açık hata vermeli —
        // ABI sözleşmesi backend'lere de açıktır.
        {
            const DataMethod* m = dataLookupMethod("int", "length", false, true);
            assert(m != nullptr);
            f.reset();
            f.args = nullptr; f.argc = 0;
            assert(rt_host_call(kBuiltinBase + dataMethodId(m), &f) != 0);
            assert(f.err.failed());
        }
    }

    // 9) Kayıt tamlığı (#229). Kayıt birliği sonrası TEK tablo (hostRegistry):
    //    her kaydın thunk'ı bağlı, arity/retKind geçerli olmalı; host
    //    fonksiyonlarının (blok 1) symbolicId'si benzersiz olmalı. Builtin
    //    adları tip başına benzersizdir (kategori ayrımı dataMethodId'de),
    //    bu yüzden global benzersizlik yalnız host bloğunda aranır.
    {
        const auto& reg = hostRegistry();
        std::set<std::string> seen;
        int checked = 0;
        for (const auto& e : reg) {
            if (!e.symbolicId) continue;   // boş blok aralıkları
            if (!e.thunk) {
                std::printf("KAYIT THUNKSUZ: %s\n", e.symbolicId);
                assert(false);
            }
            assert(e.arity >= 0);
            assert(e.retKind >= HostKind::Int && e.retKind <= HostKind::Void);
            if (hostEntryIndex(e.symbolicId) >= 0) {   // blok 1 üyesi
                if (!seen.insert(e.symbolicId).second) {
                    std::printf("KAYIT YINELI: %s\n", e.symbolicId);
                    assert(false);
                }
            }
            ++checked;
        }
        // Kayıt tamlığı sabit toplamdır: gömülü host fonksiyonları (hostFnTable +
        // dataDateFunctions) + builtin metodlar. Her FFI ekleyişinde güncellenir;
        // toplam, 256 tabanının altında olduğu sürece blok çakışması olmaz.
        assert(checked == 94 + 28);
        std::printf("kayit tamligi: %d kayit (94 host + 28 builtin)\n", checked);
    }

    std::printf("test_host_abi: TUM TESTLER GECTI\n");
    return 0;
}
