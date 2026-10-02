// ============================================================================
// saQut — Veri Tipi Registry'si (gerçekleme)
// ============================================================================

#include "data/data_registry.hpp"

#include <cctype>

#include "data/array.hpp"
#include "data/string.hpp"
#include "data/struct.hpp"

// ── Birleşik tablo ───────────────────────────────────────────────────────────
//
// SIRA KARARLIDIR (ADR-044). İndeksler IR'ye gömülür (CALLHOST::intValue); bu
// yüzden yeni kayıtlar ilgili modülün tablosunun SONUNA, yeni modüller de bu
// listenin SONUNA eklenir. Güncel id listesi: `saqut ir` CALLHOST satırları.
const std::vector<DataMethod>& dataAllMethods() {
    static const std::vector<DataMethod> all = [] {
        std::vector<DataMethod> v;
        auto append = [&v](const std::vector<DataMethod>& src) {
            v.insert(v.end(), src.begin(), src.end());
        };
        append(dataArrayMethods());
        append(dataStringMethods());
        append(dataStructMethods());
        return v;
    }();
    return all;
}

// ── Derleme zamanı arama ─────────────────────────────────────────────────────
//
// Aramalar yalnız derleme zamanında (tip denetimi, LSP) yapılır ve tablo
// birkaç düzine kayıttır: düz tarama yeterli. Aynı ad farklı kategorilerde
// bulunabilir (string.length ve E[].length); kategori bu yüzden anahtarın
// parçasıdır.

std::optional<DataMethodCategory> dataReceiverCategory(const Type& receiver) {
    if (receiver.isArray() && receiver.elementType) return DataMethodCategory::Array;
    if (receiver.isString()) return DataMethodCategory::StringVal;
    if (receiver.isStruct()) return DataMethodCategory::StructVal;
    return std::nullopt;
}

const DataMethod* dataFindMethod(DataMethodCategory category, const std::string& methodName) {
    for (const DataMethod& m : dataAllMethods())
        if (m.category == category && methodName == m.name) return &m;
    return nullptr;
}

bool dataMethodAcceptsReceiver(const DataMethod& method, const Type& receiver) {
    const DataParamRule& rule = method.params[0];
    if (rule.kind != DataParamKind::Fixed) return true;
    return receiver.equalsBase(rule.fixedType);
}

std::string dataMethodNames(DataMethodCategory category, const Type& receiver) {
    std::string names;
    for (const DataMethod& m : dataAllMethods()) {
        if (m.category != category || !dataMethodAcceptsReceiver(m, receiver)) continue;
        if (!names.empty()) names += ", ";
        names += m.name;
    }
    return names;
}

int dataMethodId(const DataMethod* m) {
    if (!m) return -1;
    const auto& all = dataAllMethods();
    // Pointer aritmetiği güvenli: m her zaman dataAllMethods() içindeki bir
    // kayda işaret eder (lookup yalnızca oradan döner).
    if (m < all.data() || m >= all.data() + all.size()) return -1;
    return (int)(m - all.data());
}

const DataMethod* dataMethodAt(int id) {
    const auto& all = dataAllMethods();
    if (id < 0 || id >= (int)all.size()) return nullptr;
    return &all[id];
}

Type dataResolveElemType(const std::string& leftName) {
    Type t = Type::fromName(leftName);
    if (!t.isError()) return t;
    // Struct tipi — fromName tanımaz ama structType ile üretilebilir.
    if (!leftName.empty() && std::isupper((unsigned char)leftName[0]))
        return Type::structType(leftName);
    return Type::error();
}
