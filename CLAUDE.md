# Claude Entry Point for saQut

Bu dosya bağımsız bir kural seti değildir. Repository'deki tek genel otorite
`AGENTS.md` dosyasıdır.

Claude ile her yeni oturumda:

1. `AGENTS.md` dosyasını tamamen oku ve uygula.
2. `knowledge-base/` altındaki bütün dosyaları oku.
3. `saQut-compiler-audit-metodolojisi.md` dosyasını oku — `AGENTS.md` §4'teki
   şüpheci denetim yönteminin uzun formu, somut vaka çalışmalarıyla.
4. `docs/v1.0-kapsam-bildirgesi.md`,
   `docs/adr/ADR-042-v1-feedback-mvp-ve-surumleme.md`,
   `docs/v0.9-v1.0-yol-haritasi.md` ve
   `docs/v1.0-issue-disposition.md` dosyalarını oku.

Tek çalışma disiplini `AGENTS.md` §4'tedir. Model adı yetki belirlemez.

## Commit kuralı

Commit mesajlarına `Co-Authored-By`, `Claude-Session`, `Generated with ...`
gibi araç/model imzası **eklenmez**. Commit mesajı yalnızca yapılan işi ve
gerekçesini anlatır. Bu kural PR açıklamaları için de geçerlidir.

## Dosya düzenleme kuralı

Mevcut dosyalar yalnız düzenleme aracıyla (Edit), yeni dosyalar yalnız yazma
aracıyla (Write) değiştirilir; ürün sahibi her değişikliği diff olarak
görebilmelidir. Dosya yazmak/düzenlemek için `python3 - <<'EOF'` heredoc'u,
`cat > dosya <<EOF` ya da `sed -i`/`awk` ile toplu değiştirme **kullanılmaz**.

Edit başarısız olursa (eşleşme yok, metin birden fazla yerde geçiyor vb.)
betiğe geçilmez: eşleşme daha fazla çevre bağlamla benzersiz yapılır ya da
değişiklik küçük parçalara bölünür. Yine çözülmüyorsa DUR ve hangi dosyada,
neden başarısız olduğunu raporla.

## Değişmez ürün bağlamı

- `0.8.0` yayınlanmış baseline'dır.
- Aktif hedefler `0.9.x` ve `1.0.0`'dır.
- Derleyiciyi stabil hale getirmek tek başına sürüm numarası atlama gerekçesi
  değildir. Yeni milestone, kullanıcı tarafından gözlenen yeni/iyileşmiş ürün
  davranışı ve bunun kanıt programlarıyla gerekçelendirilir.
- `0.9.x` sessiz stabilizasyon release'i değildir; ürün sahibinin dili gerçek
  CLI/veri-işleme akışlarında daha hareketli kullanabildiği preview olmalıdır.
- v1.0 production-ready vaat değil, Feedback MVP'dir.
- VM stabil referanstır; JIT `[EXPERIMENTAL]`dır.
- AOT ve public concurrency v1 dışıdır.
- DoD: `Tasarlandı → Uygulandı → Test Edildi → Release Edildi`.

Claude, issue/ADR/rapordaki başarı cümlelerini kanıt olarak kabul etmez.
Ürün sahibinin onayı gereken GC, concurrency, CLI, FFI, platform ve dil
ergonomisi kararlarında seçenekleri sunup durur.

`.claude/` altında gizli agent, hook veya yerel override kullanılmaz. Bu dosya
ve `AGENTS.md` ile çelişen geçmiş prompt'lar geçersizdir.
