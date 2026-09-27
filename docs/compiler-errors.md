# saQut tanı ve hata kodları

<!-- Bu dosya ÜRETİLİR: src/diagnostic/diagnostic.hpp → tests/gen_diagnostic_docs.cpp. Elle düzenlemeyin; yeniden üretmek için:
     ./build/gen_diagnostic_docs > docs/compiler-errors.md -->

Kodlar kararlıdır: `saqut check` JSONL çıktısı, LSP ve `Error.code` bu kodları taşır. Mesaj metni bağlama göre değişir; aşağıdaki başlık ve açıklama kodun genel anlamıdır.

## Derleme zamanı tanıları

| Kod | Seviye | Başlık | Açıklama |
|-----|--------|--------|----------|
| E000 | hata | Internal: AST could not be built | Parser hiç AST üretemedi ve başka tanı da vermedi. Derleyici hatasıdır; bildirin. |
| E001 | hata | Undefined name or member | Ad (değişken, fonksiyon, alan, metot, enum üyesi) tanımlı değil ya da bu noktada görünmüyor. Yazımı ve kapsamı kontrol edin. |
| E002 | hata | Duplicate definition in same scope | Aynı kapsamda aynı ad ikinci kez tanımlandı. |
| E003 | hata | Type mismatch in assignment | Değer hedefin tipine atanamıyor: değişken başlatma/atama, fonksiyon argümanı ya da dönüş değeri. Gizli dönüşüm yoktur (ADR-010); `as` ile açık dönüşüm yapın. Nullable (`T?`) değer null denetimi olmadan `T`'ye atanamaz. |
| E004 | hata | break/continue outside loop/switch | `break` yalnız döngü ya da switch içinde, `continue` yalnız döngü içinde kullanılabilir. |
| E005 | hata | return outside function | `return` fonksiyon gövdesi dışında kullanıldı. |
| E006 | hata | Missing return value | void olmayan fonksiyonun bazı yolları değer döndürmüyor ya da değersiz `return;` kullanılmış. Dönen değerin tipi uymuyorsa E003 verilir. |
| E007 | hata | Unknown type | Tip adı tanınmıyor: ilkel tip, struct, enum ya da import edilmiş bir tip değil. |
| E008 | hata | Call argument count mismatch | Fonksiyon ya da metot beklediğinden farklı sayıda argümanla çağrıldı. |
| E010 | hata | Recursive struct through non-nullable fields | Struct kendisini nullable olmayan alanlar zinciriyle içeriyor; böyle bir değer hiç kurulamaz. Zincirdeki bir alanı nullable (`T?`) yapın. |
| E011 | hata | Declaration inside a function body | struct, enum, fonksiyon ya da import bildirimi fonksiyon gövdesi içinde yapılamaz; modül düzeyine taşıyın. |
| E012 | hata | Type does not support [index] access | `[ ]` ile indeksleme yalnız dizilerde tanımlıdır. |
| E013 | hata | Statement at module scope | Modül (global) düzeyinde yalnız bildirim yazılabilir; deyimleri bir fonksiyonun (ör. main) içine taşıyın. |
| E014 | hata | shared / Pool / List declaration rule | `shared` yerel bildirimde; shared tipi int/float/bool/Pool/List değil; `Pool(T)`/`List(T)` shared global başlatıcısı dışında; ya da Pool/List değeri shared global adı dışında kullanıldı. |
| E015 | hata | Type is not sendable | Pool/List eleman tipi ya da `thread { }` yakalaması Pool/List/fonksiyon içeriyor; bu değerler thread'ler arasında kopyalanamaz. |
| E016 | hata | Assignment to a captured variable in a thread body | `thread { x = 5; }` — `x` çevreleyen fonksiyonun yereli; gövde onun bir kopyasını görür, atama dışarı yansımaz. |
| E017 | hata | lock / unlock rule | Kilit hedefi shared int/float/bool değil; aynı kilit ikinci kez alınıyor; ya da tutulmayan kilit bırakılıyor. |
| E018 | hata | wait condition has no shared symbol | `wait` koşulu hiçbir shared değişkene bakmıyor; beklerken hiç değişemez. |
| E019 | hata | wait inside a lock scope | `lock` kapsamı içinde `wait` (v1'de desteklenmez). |
| E020 | hata | Literal does not fit its context | Sayı literali hedef tipin aralığına sığmıyor (int, longint, byte) ya da ondalık literal tamsayı bağlamında kullanıldı. |
| E021 | hata | Operator not defined for these types | Aritmetik, karşılaştırma, tekli işaret ya da `++`/`--` operatörü bu işlenen tiplerinde tanımlı değil (ör. string'de `<`, decimal'de `**`, longint ile başka bir sayı tipinin karışımı). |
| E022 | hata | Nullable value used without a null check | Nullable (`T?`) değer operatöre, üye erişimine ya da `++`/`--`'ye null denetimi olmadan verildi. `if (x != null)` ile daraltın ya da önce yerel bir değişkene alın. |
| E023 | hata | Invalid 'as' conversion | `as` dönüşümü bu kaynak ve hedef tip çifti için tanımlı değil ya da hedef tip bilinmiyor. |
| E024 | hata | switch / case type mismatch | switch konusu desteklenmeyen tipte, `case` değeri konunun tipine uymuyor ya da `case null` nullable olmayan bir konuda kullanıldı. |
| E025 | hata | Value is not callable | Fonksiyon olmayan bir değer çağrıldı. |
| E026 | hata | Invalid method receiver | Metot bu alıcı tipinde tanımlı değil (ör. `toString` yalnız `byte[]`'da) ya da `array::`/`struct::` ad alanına yanlış tipte ilk argüman verildi. |
| E027 | hata | Assignable location required | Atama (`=`, `+=`, `&=`, …) ve `++`/`--` yalnız değişken, struct alanı ya da dizi elemanına uygulanabilir. |
| E_MODULE_NOT_FOUND | hata | Imported module file not found | `import ... from "yol.sqt"` dosyası bulunamadı. Yol, import eden dosyanın dizinine göre çözülür. |
| E_MODULE_PARSE | hata | Imported module could not be parsed | İçe aktarılan dosyadan AST kurulamadı. |
| E_MODULE_CYCLE | hata | Circular module dependency | Modüller birbirini döngüsel olarak içe aktarıyor; mesaj zinciri gösterir. |
| E_IMPORT_UNKNOWN | hata | Unknown module or imported name | Gömülü modül (math, fs, …) ya da içe aktarılan ad bulunamadı. |
| E_IMPORT_NOT_EXPORTED | hata | Imported name is not exported | Ad kaynak modülde tanımlı ama `export` ile işaretlenmemiş. |
| E_SYMBOL_NOT_IMPORTED | hata | Symbol used without import | Başka bir modüle ait ad bu dosyada `import` edilmeden kullanıldı ya da farklı bir adla içe aktarıldı. |
| W002 | uyarı | Division by zero in a constant expression | Sabit katlama sıfıra bölme buldu; çalışma zamanında E_DIVZERO atılır. |
| W003 | uyarı | Unreachable code | return/break/continue'dan sonra gelen deyimler hiç çalışmaz ve derlemeden çıkarılır. |
| W004 | uyarı | Implicit numeric widening | Sayı daha geniş bir tipe (int → float/double/decimal) örtük olarak genişletildi. |
| W005 | uyarı | Float case value is not exactly representable | `case` ondalık değeri IEEE 754'te tam temsil edilemiyor; eşitlik karşılaştırması beklenen sonucu vermeyebilir. |
| W006 | uyarı | Deprecated builtin call syntax (ADR-033) | Eleman tipi önekli eski metot sözdizimi (`int::push(a, x)`); `a.push(x)` ya da `array::push(a, x)` kullanın. |
| W007 | uyarı | Ignored capability requirement (ADR-043) | `requires <yetenek>` yok sayılır: capability sistemi kaldırıldı. |
| W008 | uyarı | Non-atomic update of a shared variable | `x = x + 1` / `x *= 2` ayrı yükle + yaz yapar; `+=`, `-=`, `++`, `--` atomiktir. |
| W009 | uyarı | Blocking call inside a lock scope | Kilit tutulurken bekleyen bir çağrı (ör. `pop`, `join`) yapılıyor. |
| E901 | hata | Syntax error: unexpected token | Bu noktada beklenmeyen bir token var; deyim atlanıp bir sonraki sınırdan devam edilir. |
| E902 | hata | Syntax error: expected type name after 'as' | `as` sonrasında bir tip adı bekleniyor. |
| E903 | hata | Syntax error: expected member name | `.` ya da `->` sonrasında üye adı bekleniyor. |
| E904 | hata | Syntax error: expected a name | Değişken, parametre, fonksiyon ya da alan adı bekleniyor (ör. bir keyword ad olarak kullanıldı). |
| E905 | hata | Syntax error: expected closing delimiter | Kapanış sınırlayıcısı (`;`, `)`, `]`, `}`) eksik. |
| E906 | hata | Unknown escape sequence in string literal | Desteklenen kaçışlar: \n \t \r \b \\ \". |
| E907 | hata | Unterminated string literal | String literali kapanış `"` bulunmadan dosya sonuna ulaştı. |

## Çalışma zamanı hata kodları (`Error.code`)

Program `try { ... } catch (Error e) { ... }` ile yakalayıp `e.code`'a bakabilir. Yakalanmayan hata programı çıkış kodu 70 ile bitirir.

| Kod | Başlık | Açıklama |
|-----|--------|----------|
| `E_DIVZERO` | Division or modulo by zero | int / longint / float bölme ya da mod işleminde bölen sıfır. |
| `E_DECIMAL_DIVZERO` | Decimal division or modulo by zero | decimal bölme ya da mod işleminde bölen sıfır. |
| `E_DECIMAL_OVERFLOW` | Decimal overflow | decimal işleminin sonucu temsil aralığını aştı. |
| `E_POWNEG` | Negative integer exponent | Tamsayı `**` işleminde üs negatif; sonuç tamsayı olamaz. |
| `E_CAST` | Checked conversion failed | `as` dönüşümü değeri hedef tipe sığdıramadı (aralık dışı, NaN/Inf, sayı olmayan metin). `as T?` biçimi hata yerine null verir. |
| `E_OOB` | Array index out of bounds | Dizi indeksi 0 ile uzunluk-1 aralığının dışında. |
| `E_NULL` | Null reference access | null struct/dizi değerinin alanına ya da elemanına erişildi. |
| `E_TYPE` | Unexpected runtime value type | İşlem beklediği değer türünü (ör. dizi) almadı. |
| `E_STACK_OVERFLOW` | Maximum call depth exceeded | Özyineleme `--max-call-depth` sınırını aştı (varsayılan 100000). |
| `E_HOST` | Built-in or host function failed | Bir built-in metot ya da FFI host fonksiyonu hata bildirdi (ör. dosya açılamadı, geçersiz argüman); ayrıntı mesajdadır. |
| `E_LIST_INDEX` | List index out of range | `List.get(i)` için i < 0 ya da i >= length(). |
| `E_LIST_FULL` | List is full | List kapasitesi (≈16,7 milyon eleman) doldu. |

Kodu olmayan ölümcül çalışma zamanı durumu: bütün thread'ler bloklandığında (deadlock) "runtime error: all threads are blocked (deadlock)" ve her thread'in beklediği yer basılır; çıkış kodu 70.
