// ============================================================================
// saQut FFI — fs host fonksiyonları (#87, #115)
// ============================================================================
//
// Dosya içeriği her zaman byte[] olarak taşınır.
// v1: yol güvenliği yok (hepsi-ya-hiçbir-şey). Handle/descriptor
// YOK — tek atımlık read/write (record-replay v1.2.0 önkoşulu, ADR-034 §5).
// copy/rename/create/isEmpty/isDirectory/fileSize (ürün kararı 2026-08-25,
// #115) + createDirectory/removeDirectory/list/walk/isFile/modifiedTime
// (#115 Faz 1) std::filesystem üzerinden çalışır; fail → E_HOST; dizin/yokluk
// sorguları (isFile/isDirectory/isEmpty) fail ETMEZ (bool döner).
// move dosya için renameFile'dır (std::filesystem::rename = taşıma) — ayrı
// moveFile eklenmez (#115 "move" böyle çözülür, AGENTS.md §10.2 tek tanım).
// ============================================================================

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "ffi/host_functions.hpp"
#include "ffi/host_bridge.hpp"

static int fs_readFile(HostCallFrame* fr) {
    const std::string& path = hostAsString(fr->args[0]);
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) { fr->err.set("cannot open file '" + path + "'", "E_HOST"); return 1; }
    if (!fr->env || !fr->env->heap) {
        fr->err.set("readFile: heap yok", "E_HOST");
        return 1;
    }
    // İsteğe bağlı seek/size: null/eksik → baştan okur / sonuna kadar okur.
    // Büyük dosyalarda tümünü kopyalamadan doğrudan ArrayObject baytlarına
    // yazarız (tek uygulanmış kopya, #115 seek isteği ürün kararı 2026-08-26).
    int64_t offset = 0;
    int64_t length = -1;   // -1 = sonuna kadar
    if (fr->argc >= 2 && !fr->args[1].isNull())
        offset = std::max<int64_t>(0, hostAsI64(fr->args[1]));
    if (fr->argc >= 3 && !fr->args[2].isNull())
        length = std::max<int64_t>(0, hostAsI64(fr->args[2]));

    // Boyut üst sınırı: offset aşarsa boş dizin okunur; length seçilirse tam
    // ofset+boyut aralığı okunur, aksi halde sona kadar. file_size başarısız
    // olursa (bu aşamada dosya açıldığı için nadiren) sona-kadar oku.
    std::error_code ec;
    uintmax_t total = std::filesystem::file_size(path, ec);
    uintmax_t totalValid = ec ? 0 : total;
    if (f.good()) f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    const int64_t remaining =
        static_cast<int64_t>(totalValid > static_cast<uintmax_t>(offset) ? totalValid - static_cast<uintmax_t>(offset) : 0);
    int64_t n = (length >= 0) ? std::min(length, remaining) : remaining;
    if (n < 0) n = 0;

    ArrayObject* arr = fr->env->heap->allocArray(static_cast<int>(n), ArrayElemKind::Byte);
    if (n > 0) {
        arr->bytes.resize(static_cast<size_t>(n));
        f.read(reinterpret_cast<char*>(arr->bytes.data()), n);
        const std::streamsize got = f.gcount();
        arr->bytes.resize(static_cast<size_t>(got));
    }
    fr->ret = HostSlot::fromRef(arr);
    return 0;
}

static int fs_writeFile(HostCallFrame* fr) {
    const std::string& path = hostAsString(fr->args[0]);
    std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { fr->err.set("cannot open file '" + path + "' for writing", "E_HOST"); return 1; }
    if (fr->args[1].kind != HostKind::Ref || !fr->args[1].p) {
        fr->err.set("writeFile: expected byte[]", "E_HOST");
        return 1;
    }
    auto* arr = static_cast<ArrayObject*>(fr->args[1].p);
    if (arr->elemKind != ArrayElemKind::Byte) {
        fr->err.set("writeFile: expected byte[]", "E_HOST");
        return 1;
    }
    f.write(reinterpret_cast<const char*>(arr->bytes.data()), arr->bytes.size());
    fr->ret = HostSlot::voidVal();
    return 0;
}

static int fs_append(HostCallFrame* fr) {
    const std::string& path = hostAsString(fr->args[0]);
    std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::app);
    if (!f.is_open()) { fr->err.set("cannot open file '" + path + "' for writing", "E_HOST"); return 1; }
    if (fr->args[1].kind != HostKind::Ref || !fr->args[1].p) {
        fr->err.set("append: expected byte[]", "E_HOST");
        return 1;
    }
    auto* arr = static_cast<ArrayObject*>(fr->args[1].p);
    if (arr->elemKind != ArrayElemKind::Byte) {
        fr->err.set("append: expected byte[]", "E_HOST");
        return 1;
    }
    f.write(reinterpret_cast<const char*>(arr->bytes.data()), arr->bytes.size());
    fr->ret = HostSlot::voidVal();
    return 0;
}

static int fs_exists(HostCallFrame* f) {
    f->ret = HostSlot::fromInt(std::filesystem::exists(hostAsString(f->args[0])) ? 1 : 0);
    return 0;
}

static int fs_remove(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    bool removed = std::filesystem::remove(path, ec);
    if (ec)       { f->err.set("cannot remove '" + path + "': " + ec.message(), "E_HOST"); return 1; }
    if (!removed) { f->err.set("file not found: '" + path + "'", "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_createFile(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { f->err.set("cannot create file '" + path + "'", "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_copyFile(HostCallFrame* f) {
    const std::string& src = hostAsString(f->args[0]);
    const std::string& dst = hostAsString(f->args[1]);
    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { f->err.set("cannot copy '" + src + "' to '" + dst + "': " + ec.message(), "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_renameFile(HostCallFrame* f) {
    const std::string& from = hostAsString(f->args[0]);
    const std::string& to   = hostAsString(f->args[1]);
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) { f->err.set("cannot rename '" + from + "' to '" + to + "': " + ec.message(), "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_isEmpty(HostCallFrame* f) {
    // Var olmayan yol → boş sayılmaz (false); hata değil.
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    bool empty = false;
    if (std::filesystem::exists(path, ec)) {
        if (std::filesystem::is_directory(path, ec))
            empty = std::filesystem::directory_iterator(path, ec) == std::filesystem::directory_iterator{};
        else if (std::filesystem::exists(path, ec))
            empty = std::filesystem::file_size(path, ec) == 0;
    }
    f->ret = HostSlot::fromInt(empty ? 1 : 0);
    return 0;
}

static int fs_isDirectory(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    bool isDir = std::filesystem::is_directory(path, ec);
    f->ret = HostSlot::fromInt(isDir ? 1 : 0);
    return 0;
}

static int fs_fileSize(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec) { f->err.set("cannot stat '" + path + "': " + ec.message(), "E_HOST"); return 1; }
    f->ret = HostSlot::fromLong(static_cast<int64_t>(sz));
    return 0;
}

// ── #115 Faz 1 dizin yönetimi ───────────────────────────────────────────────
// createDirectory tek dizin oluşturur (mevcut/yüklü değilse fail); yoksa
// varolan dizin hatasız döner mi? — std::filesystem::create_directory varolan
// dizinde false döner, hata değil. removeDirectory RECURSIVE siler (remove_all)
// — boş olmayan dizinler dahil; yokluk → E_HOST.

static int fs_createDirectory(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    std::filesystem::create_directory(path, ec);  // varolan dizinde false döner, hata değil
    if (ec) { f->err.set("cannot create directory '" + path + "': " + ec.message(), "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_removeDirectory(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    bool removed = std::filesystem::remove_all(path, ec) > 0;
    if (ec) { f->err.set("cannot remove directory '" + path + "': " + ec.message(), "E_HOST"); return 1; }
    if (!removed) { f->err.set("directory not found: '" + path + "'", "E_HOST"); return 1; }
    f->ret = HostSlot::voidVal();
    return 0;
}

static int fs_isFile(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    bool isF = std::filesystem::is_regular_file(path, ec);
    f->ret = HostSlot::fromInt(isF ? 1 : 0);
    return 0;
}

// list: tek dizinin hemen altındaki adlar (düzenli dosya + alt dizin), yalnız
// isim (tam yol değil), deterministik olarak sıralı. walk: recursive, tam yol.
static int collect_entries(HostCallFrame* f, bool recursive) {
    if (!f->env || !f->env->heap) { f->err.set("list: heap yok", "E_HOST"); return 1; }
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    std::vector<std::string> out;
    if (recursive) {
        std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
        if (ec) { f->err.set("walk '" + path + "': " + ec.message(), "E_HOST"); return 1; }
        while (it != end) { out.push_back(it->path().string()); it.increment(ec); if (ec) break; }
    } else {
        std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
        if (ec) { f->err.set("list '" + path + "': " + ec.message(), "E_HOST"); return 1; }
        for (; it != end; it.increment(ec)) { if (ec) break; out.push_back(it->path().filename().string()); }
    }
    std::sort(out.begin(), out.end());
    ArrayObject* arr = f->env->heap->allocArray((int)out.size());
    for (const auto& s : out) arr->elements.push_back(Value::fromString(s));
    f->ret = HostSlot::fromRef(arr);
    return 0;
}

static int fs_list(HostCallFrame* f) { return collect_entries(f, false); }
static int fs_walk(HostCallFrame* f) { return collect_entries(f, true); }

// modifiedTime — son yazma zamanı, epoch milisaniye (longint); date değil.
static int fs_modifiedTime(HostCallFrame* f) {
    const std::string& path = hostAsString(f->args[0]);
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) { f->err.set("cannot stat '" + path + "': " + ec.message(), "E_HOST"); return 1; }
    auto stime = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::filesystem::file_time_type::clock::to_sys(ftime));
    f->ret = HostSlot::fromLong(stime.time_since_epoch().count());
    return 0;
}

// ── Tablo (fs alt kümesi) ───────────────────────────────────────────────────
const std::vector<HostFn>& fsHostFunctions() {
    static const std::vector<HostFn> table = {
        { "FS_READ_FILE", 3, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref, fs_readFile },
        { "FS_WRITE_FILE", 2, HOST_CAN_FAIL, HostKind::Void, fs_writeFile },
        { "FS_APPEND", 2, HOST_CAN_FAIL, HostKind::Void, fs_append },
        { "FS_EXISTS",     1, 0, HostKind::Int, fs_exists },
        { "FS_REMOVE", 1, HOST_CAN_FAIL, HostKind::Void, fs_remove },
        { "FS_CREATE_FILE", 1, HOST_CAN_FAIL, HostKind::Void, fs_createFile },
        { "FS_COPY_FILE", 2, HOST_CAN_FAIL, HostKind::Void, fs_copyFile },
        { "FS_RENAME_FILE", 2, HOST_CAN_FAIL, HostKind::Void, fs_renameFile },
        { "FS_IS_EMPTY",     1, 0, HostKind::Int, fs_isEmpty },
        { "FS_IS_DIRECTORY", 1, 0, HostKind::Int, fs_isDirectory },
        { "FS_FILE_SIZE",    1, HOST_CAN_FAIL, HostKind::LongInt, fs_fileSize },
        { "FS_CREATE_DIRECTORY", 1, HOST_CAN_FAIL, HostKind::Void, fs_createDirectory },
        { "FS_REMOVE_DIRECTORY", 1, HOST_CAN_FAIL, HostKind::Void, fs_removeDirectory },
        { "FS_IS_FILE",     1, 0, HostKind::Int, fs_isFile },
        { "FS_LIST",        1, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref, fs_list },
        { "FS_WALK",        1, HOST_NEEDS_HEAP | HOST_CAN_FAIL, HostKind::Ref, fs_walk },
        { "FS_MODIFIED_TIME", 1, HOST_CAN_FAIL, HostKind::LongInt, fs_modifiedTime },
    };
    return table;
}
