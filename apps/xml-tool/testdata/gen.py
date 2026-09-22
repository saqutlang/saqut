#!/usr/bin/env python3
"""xml-tool ölçek test verisi üreticisi.

Üretilen dosyalar repoda tutulmaz (boyut + üretilmiş çıktı kuralı):
    derin.xml   — iç içe derinlik (özyineleme yolu)
    genis.xml   — yatay genişlik (metin API'si maliyeti)

Kullanım:  python3 gen.py [derin_derinlik] [genis_urun_sayisi]
"""
import sys

deep = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
wide = int(sys.argv[2]) if len(sys.argv) > 2 else 20000

with open("derin.xml", "w") as f:
    f.write("<root>")
    f.write("<n>" * deep)
    f.write("dip")
    f.write("</n>" * deep)
    f.write("</root>\n")

with open("genis.xml", "w") as f:
    f.write('<?xml version="1.0"?>\n<katalog>\n')
    for i in range(wide):
        f.write(f'  <urun id="u{i}"><ad>Urun {i}</ad><fiyat>{i * 7}</fiyat></urun>\n')
    f.write('</katalog>\n')

print(f"derin.xml: {deep} derinlik, genis.xml: {wide} urun")
