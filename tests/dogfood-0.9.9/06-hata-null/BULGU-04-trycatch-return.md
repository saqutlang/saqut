# Bulgu 04 — `try`/`catch` ikisi de `return` etse bile E006 veriyor

**Adım:** 06-hata-null / try-catch ile güvenli bölme yazarken
**Ölçüm:** `0.9.9`, commit `1fa6562`, Release
**Backend farkı yok:** derleme zamanı, VM/JIT ikisinde de aynı.

## Ne oldu

Hatayı yutup varsayılan değer döndüren, son derece sıradan bir fonksiyon:

```c
int guvenliBol(int a, int b) {
    try {
        return a / b;
    } catch (Error e) {
        return -1;
    }
}
```

```
error [E006]: 'guvenliBol' function must return int but some paths have no return
    hint: add `return <value>;` to all control flow paths
```

**Her iki yol da `return` ediyor.** Üçüncü bir yol yok.

## Karşılaştırma: `if/else` kabul ediliyor

```c
int f(int a, int b) {
    if (b == 0) { return -1; } else { return a / b; }   // ✅ sorunsuz
}
```

Aynı yapı, aynı garanti — ama `try/catch` reddediliyor.

## Kök neden

`type_checker.cpp:135` `pathAlwaysReturns()`:

```cpp
case ASTKind::IfStatement:     { ... }   // ele alınmış
case ASTKind::SwitchStatement: { ... }   // ele alınmış (#165/#136)
default:
    return false;   // ← TryStatement buraya düşüyor
```

`ASTKind::TryStatement` için case **yok**. Fonksiyon `If` ve `Switch`'i
biliyor (switch'e `default` varsa ve her case return ediyorsa doğru cevabı
veriyor), ama `try`'ı tanımıyor.

Doğru kural `if/else` ile birebir aynı olurdu: **try bloğu VE catch bloğu
ikisi de garantili dönüyorsa, try/catch garantili döner.** (`finally` varsa
o da hesaba katılmalı — dilde var mı kontrol edilmeli.)

## Kullanıcının ödediği bedel

Tek çıkış yolu ulaşılamaz bir satır yazmak:

```c
int guvenliBol(int a, int b) {
    try { return a / b; }
    catch (Error e) { return -1; }
    return 0;      // ← ULAŞILAMAZ, ama derleyici olmadan derlemiyor
}
```

Bu satır hiçbir zaman çalışmaz. Üstelik **ulaşılamaz kod uyarısı da
verilmiyor** (`saqut check` temiz: 0 error, 0 warning). Yani derleyici hem
gereksiz kodu zorunlu kılıyor hem de gereksiz olduğunu söylemiyor.

Bu, "hata yakalayıp varsayılan dönen fonksiyon" gibi en yaygın try/catch
kalıbını yazan herkesi etkiler.

## Etki

try/catch'in birincil kullanım biçimi bu. Dili kullanan biri ilk
try/catch'li fonksiyonunda bu duvara çarpar ve ya ölü kod yazar ya da
try/catch'i bırakıp `if` ile elle kontrol etmeye döner.

## Kanıt sınıfı

Doğrulanan: yukarıdaki üç program (reddedilen, kabul edilen if/else
karşılaştırması, ulaşılamaz-return workaround'u) koşuldu; kök neden
`pathAlwaysReturns`'te case eksikliği olarak kaynakta görüldü; workaround'un
uyarı üretmediği `saqut check` ile doğrulandı.
Doğrulanmayan: dilde `finally` olup olmadığı ve varsa kuralın nasıl
kurulacağı; `try` içinde `throw` eden bir catch'in (yeniden fırlatma)
davranışı bu açıdan denenmedi.
