# Soul — 2. tur planı: LZX'li .msi paketleri ve Direct3D 9

Hedef: (1) LZX ile sıkıştırılmış cab dosyalarını açarak gerçek .msi kurulumlarının çoğunu (Visual C++ runtime'ları,
pek çok ticari program) kurabilmek; (2) Direct3D 9 kullanan programların ve oyunların çalışması — bu turda
doğruluk için CPU'daki yazılım GPU'su (sgpu) üzerinde; hızlı Metal arka ucu 3. turda.
Kapsam dışı (sonraki turlar): Metal arka ucu, D3DX9 (oyunların getirdiği d3dx9_xx.dll gerçek DLL olarak yüklenir;
kendi uygulamamız yok), Direct3D 10/11, .NET, Quantum sıkıştırma.

Önceki tur (MSI, GDI+, XAudio2) tamamlandı: commit 8986bab, CI tümüyle yeşil.

## Gruplar ve sıra

```
G1 (LZX) ───────────────────────────────────────────────────────────────┐
G0 (sgpu genişletme + gölgelendirici arayüzü) ──┬──> G2 (D3D9 API) ─────┼──> G4 (entegrasyon)
                                                └──> G3 (D3D9 gölgelendiriciler) ┘
```
G0 ile G1 paralel; G0 bitince G2 ve G3 paralel. Her grup kendi worktree'sinde, yalnızca kendi dosyalarında.

---

## G0 — sgpu genişletme ve gölgelendirici arayüzü (önce)
Dosyalar: `src/sgpu.h`, `src/sgpu.c`, yeni `src/d3d9_shader.h` (yalnızca arayüz), `tests/unit/sgpu_test.c`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 0.1 | sgpu'ya Direct3D 9'un ihtiyaçları: 8 doku aşaması, genel "varying" öznitelikleri (≥ 10 × vec4, perspektif doğru), piksel başına programlanabilir aşama için geri çağrı (girdi: varying'ler + türevleri, çıktı: renk/derinlik/discard), şablon (stencil) tamponu ve işlemleri, ayrı alfa karışımı ve karışım işlemleri (add/sub/revsub/min/max), renk yazma maskesi, eğimli derinlik sapması, kullanıcı kırpma düzlemleri, nokta sprite'ları, sarma/ayna/sınır adresleme, örnekleme fonksiyonu `sg_sample()` (mip seçimi türevlerle). OpenGL ön yüzü değişmeden çalışır. | `sgpu_test` (yerel birim testi): stencil, karışım işlemleri, varying enterpolasyonu ve piksel geri çağrısı beklenen pikselleri verir; OpenGL GUI testleri (Küp 3B dahil) aynı. |
| 0.2 | `d3d9_shader.h`: G2 ile G3 arasındaki sözleşme — `d3dsh_parse()` (SM 1.1–3.0 bayt kodu → derlenmiş yapı), `d3dsh_run_vs()` (köşe başına), `d3dsh_ps_callback` (sgpu piksel geri çağrısı), sabit/örnekleyici bağlama yapıları, girdi/çıktı semantikleri (POSITION, COLOR, TEXCOORDn, …). Boş uygulama iskeleti `src/d3d9_shader.c`. | `make` uyarısız; tüm takımlar geçer. |

## G1 — LZX sıkıştırma (G0 ile paralel)
Dosyalar: `src/cab.c`, `tests/msi/lzx/` (kodlayıcı aracı, örnek cab ve msi), `tests/unit/msidb_test.c`, `tests/unit/run_unit_tests.sh`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 1.1 | `cab.c`: LZX çözücü (pencere 2^15–2^21; verbatim, aligned, uncompressed bloklar; E8 dönüşümü; bloklar arası durum; tekrar mesafeleri). Bozuk veride çökme yok. | ASan/UBSan altında bozulma denemeleri temiz. |
| 1.2 | Test verisi: Python ile basit bir LZX kodlayıcı (`tests/msi/lzx/lzxenc.py`; literal + eşleşme, verbatim ve aligned bloklar, E8 içeren x86 kodu) + `cabextract` (libmspack) ile çapraz doğrulama; pencere boyutları 15/16/21. Bu cab'ler gömülü bir `lzx.msi` paketine konur (msibuild). | Çözülen dosyalar kaynakla ve `cabextract` çıktısıyla bayt bayt aynı; `lzx.msi` sessiz kurulup kaldırılır (win32 msi testine bir bölüm). |

## G2 — Direct3D 9 API (G0'dan sonra, G3 ile paralel)
Dosyalar: yeni `src/d3d9.c` (+ gerekirse `src/d3d9_res.c`, `src/d3d9_priv.h`), `tests/win32/d3d9.c`, `tests/win32/d3d9.expected`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 2.1 | `Direct3DCreate9(Ex)`, IDirect3D9(Ex): bağdaştırıcı bilgisi, yetenekler (D3DCAPS9, SM 3.0), ekran kipleri, biçim denetimi. IDirect3DDevice9(Ex) (tüm vtable): takas zinciri ve `Present` → pencerenin çerçeve tamponu, pencereli/tam ekran (pencereye ölçekli), `Reset`, cihaz kaybı yok. | Test programı cihaz oluşturur, `Clear` + `Present` sonrası pencere pikselleri doğru. |
| 2.2 | Kaynaklar: yüzeyler, dokular (2D/küp/hacim, mip'ler), köşe/indis tamponları, `Lock/Unlock`, biçimler (A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, L8, A8L8, P8, DXT1–5, D16/D24S8…), `StretchRect`, `UpdateSurface/Texture`, `GetRenderTargetData`, `ColorFill`, render hedefi/derinlik değiştirme. | Kilitle/yaz/oku gidiş-dönüşleri ve DXT çözme referans değerlerle aynı. |
| 2.3 | Durumlar ve çizim: render/örnekleyici/doku aşaması durumları, dönüşümler, ışıklandırma + malzeme (sabit işlev T&L), FVF ve köşe bildirimleri, `DrawPrimitive(UP)`, `DrawIndexedPrimitive(UP)`, state block'lar, sorgular (basit), `SetShader*`/sabitler → G3 arayüzü. | Test: sabit işlevli dokulu + ışıklı küp, alfa karışımı, stencil, sis — belirli piksellerde beklenen renkler (64/32-bit, JIT). |

## G3 — Direct3D 9 gölgelendiricileri (G0'dan sonra, G2 ile paralel)
Dosyalar: `src/d3d9_shader.c`, `tests/unit/d3dsh_test.c` (+ `tests/unit/shaders/` derlenmiş bayt kodları).

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 3.1 | SM 1.1–3.0 bayt kodu ayrıştırıcı: tüm aritmetik komutlar, değiştiriciler (_sat, _x2, _bias, …), kaynak değiştiriciler/swizzle, yazma maskeleri, göreli adresleme, tanımlı sabitler (def/defi/defb), dcl'ler, akış denetimi (if/else, loop, rep, break, call/ret), `texld/texldl/texldd/texkill`, ps 1.x doku komutları (tex, texcoord, texbem…). | `d3dsh_test`: hazır derlenmiş gölgelendiricilerin (fxc ile üretilmiş, depoya eklenmiş bayt kodu) bilinen girdilerde çıktıları beklenen değerler. |
| 3.2 | Yorumlayıcı: köşe gölgelendiricisi köşe başına, piksel gölgelendiricisi sgpu geri çağrısıyla piksel başına; türevler (dsx/dsy) varying türevlerinden. Hız için komutlar önceden çözülmüş bir diziye derlenir. | Birim testleri + G2 testinin gölgelendiricili bölümü (G4'te). |

## G4 — Entegrasyon (G1, G2, G3'ten sonra)
Dosyalar: `src/hle_core.c` (kayıt), `tests/win32/run_win32_tests.sh`, `tests/win32/d3d9.c` (gölgelendiricili sahne), `App/Samples` + örnek D3D9 programı, `App/Models/WinProgram.swift` (uyumluluk notu), `README.md`, CI.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 4.1 | D3D9 tablolarının kaydı, gölgelendiricili sahne testi (vs_2_0 + ps_2_0 dokulu/ışıklı, vs_3_0/ps_3_0 döngülü), örnek program "Küp 3B (Direct3D 9)". | Yerel + JIT: tüm takımlar geçer; örnek program simülatör testinde açılır. |
| 4.2 | Uygulama: d3d9 kullanan programlarda "Direct3D desteklenmiyor" notu kaldırılır (d3d10/11/12 için kalır); README ve Hakkında güncellenir. | CI yeşil (Linux + IPA + simülatör). |

## Denetim (Opus)
Her grup sonrası `git diff` incelemesi ve testler; sonunda kabul kriterleri tablosu.
