# Bulgu 03 — Derin rekürsiyonda JIT segfault ediyor, VM sınırsız büyüyor

**Adım:** 03-akis / 02_fonksiyonlar.sqt (rekürsiyon denerken)
**Ölçüm:** `0.9.9`, commit `031cc2a`, Release, `ulimit -s` = 8192 (8 MB)
**VM ≢ JIT:** evet, ayrışıyorlar.

## Ne oldu

```c
int derin(int n) { if (n <= 0) { return 0; } return 1 + derin(n - 1); }
int main() { print(derin(1000000)); return 0; }
```

```
$ saqut run derin.sqt
1000000                    ← VM tamamlıyor

$ saqut run --jit derin.sqt
Segmentation fault (core dumped)    rc=139
```

## Eşik

| Derinlik | VM | JIT |
|---|---|---|
| 100.000 | ✅ | ✅ |
| 200.000 | ✅ | ✅ |
| 250.000 | ✅ | ✅ |
| **270.000** | ✅ | **SIGSEGV (rc=139)** |
| 1.000.000 | ✅ | SIGSEGV |
| 5.000.000 | ✅ | (denenmedi) |

JIT eşiği ~250k–270k arasında. VM'de 5M denendi, hâlâ çalışıyor.

## Kök neden: JIT native C yığınını kullanıyor

`ulimit -s` iki katına çıkarılınca eşik de kalkıyor:

```
$ ulimit -s 16384 && saqut run --jit derin270k.sqt
270000          ← rc=0, aynı program geçiyor
```

Yani JIT'te her saQut çağrısı bir native çağrıya karşılık geliyor ve işletim
sisteminin yığın sınırına takılıyor. VM ise kendi `callStack_`'ini heap'te
tutuyor, o yüzden OS sınırından bağımsız.

## İki ayrı sorun

**1. JIT sessizce ölüyor.** SIGSEGV yakalanabilir bir hata değil:
`try/catch` ile tutulamaz, tanı üretmez, çıktı vermez. Kullanıcı yalnız
"Segmentation fault" görür. En azından `E_STACKOVERFLOW` gibi düzgün bir
runtime hatası olmalı.

**2. VM'in hiç sınırı yok.** `interpreter.cpp`'de derinlik kontrolü
bulunmuyor (`kMaxCallDepth` yok). Sonsuz rekürsiyon VM'de programı
yavaş yavaş belleği tüketerek OOM'a götürür. Kontrollü bir hata daha iyi
olurdu.

Bu ikisi birlikte **VM ≢ JIT sözleşmesini de bozuyor**: aynı program bir
backend'de çalışıp diğerinde çöküyor.

## Neden test paketi görmedi

`tests/golden` altında rekürsiyon fixture'ları var (fibonacci vb.) ama
hepsi sığ. Derinlik sınırını zorlayan fixture yok. #213 ("rekürsif
fonksiyonlar stack/return/GC ve determinism sınırlarını ölçsün") tam olarak
bunu istiyor ve hâlâ açık.

## Sorular (ürün sahibi)

1. Rekürsiyon derinliği için **tanımlı bir sınır** olacak mı? Varsa kaç, ve
   aşılınca yakalanabilir hata mı (`try/catch` ile tutulabilir) yoksa
   programı sonlandıran ölümcül hata mı?
2. VM ve JIT aynı sınırı mı paylaşacak? (Determinizm sözleşmesi bunu
   gerektiriyor gibi duruyor: aynı program iki backend'de aynı davranmalı.)
3. JIT'in native yığın kullanımı tasarım gereği mi, yoksa ileride kendi
   yığınına mı taşınacak? Bu, sınırın nerede konabileceğini belirliyor.

## Kanıt sınıfı

Doğrulanan: yukarıdaki eşik tablosu (her satır koşuldu), `ulimit -s` ile
eşiğin kalktığı, VM'de derinlik kontrolü bulunmadığı (kaynak taraması).
Doğrulanmayan: JIT'in tam eşiği (250k–270k arası daraltılmadı, 1k
hassasiyetle aranmadı); farklı `ulimit` değerlerinde eşiğin lineer ölçeklenip
ölçeklenmediği; çerçeve başına kaç bayt harcandığı ölçülmedi; try/catch
içeren rekürsif fonksiyonda davranış denenmedi.
