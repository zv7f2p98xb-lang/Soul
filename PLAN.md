# Soul — 3. tur planı: Metal ile GPU hızlandırma

Hedef: OpenGL ve Direct3D 9 çizimlerini iPad'in GPU'sunda (Metal) yapmak; yazılım çizici (sgpu CPU) yedek
olarak kalır. Bu turda **piksel işleme GPU'ya taşınır** (en pahalı kısım: doku örnekleme, gölgelendiriciler,
karışım); köşe işleme şimdilik CPU'da kalır (D3D9 köşe gölgelendiricisi yorumlayıcısı ~9 M köşe/s, sabit işlevli
T&L C kodu). GPU'da köşe gölgelendiricileri ve sıfır kopyalı ekrana aktarım sonraki turda.

Önceki turlar: 1. tur MSI/GDI+/XAudio2, 2. tur LZX + Direct3D 9 (yazılımla) — commit 8943aa1, CI yeşil.

**Önemli kısıt:** Bu kapsayıcıda macOS/Metal yok. Metal kodu yalnızca CI'deki macOS işlerinde derlenip test
edilebilir; bu yüzden Metal grupları (G2, G3) "gönder → CI → günlük → düzelt" döngüsüyle ilerler (gönderimi ben
yaparım). Linux'ta test edilebilen her şey (arayüz, çevirici) Linux'ta tam test edilir.

## Gruplar ve sıra

```
G0 (arka uç arayüzü + programlar) ──┬──> G1 (gölgelendirici çevirici: IR → MSL + C) ──┐
                                    ├──> G2 (Metal arka ucu, Objective-C)  ───────────┼──> G4 (entegrasyon)
                                    └──> G3 (macOS test düzeneği + CI) ────────────────┘
```

---

## G0 — sgpu arka uç arayüzü (önce)
Dosyalar: `src/sgpu.h`, `src/sgpu.c` (CPU arka ucu), yeni `src/sgpu_program.h`, `src/opengl.c`, `src/d3d9_draw.c`, `src/d3d9_res.c`, `tests/unit/sgpu_test.c`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 0.1 | `SgBackend` işlev tablosu: hedef (renk/derinlik/şablon yüzeyleri arka uca ait tutamaklar), doku oluştur/güncelle/sil (sürüm sayacıyla), çizim (köşe dizisi + durum + program), temizleme, CPU'ya eşitleme (`Lock`, `GetRenderTargetData`, `Present`, `glReadPixels` öncesi), kare sonu. Mevcut CPU kodu "cpu" arka ucu olur; `sg_backend_set()` ile değiştirilir. | Tüm takımlar bit bit aynı (unit, 52, win32 31, gui 12, msi 6; JIT). |
| 0.2 | `SgProgram`: piksel aşamasının GPU'ya çevrilebilir tanımı — (a) OpenGL sabit işlevli doku ortamı, (b) D3D9 doku aşaması zinciri (TSS), (c) D3D9 piksel gölgelendiricisi (bayt kodu + sabitler + örnekleyici bağları) ve sis/alfa testi gibi ekler. CPU arka ucu bunu mevcut `pixel_fn` yoluyla çalıştırır; ön yüzler (opengl.c, d3d9_draw.c) artık program tanımını doldurur. Bir programı çevirmeyen arka uç için çizim başına CPU'ya geri düşme yolu (hedef eşitlenir). | Aynı; ek olarak `sgpu_test`'e arka uç değiştirme ve geri düşme testleri. |

## G1 — Gölgelendirici çevirici (G0'dan sonra; Linux'ta tam test)
Dosyalar: yeni `src/sgsl.c`, `src/sgsl.h` (IR + yazıcılar), `tests/unit/sgsl_test.c`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 1.1 | `SgProgram` → ara gösterim (IR): D3D9 ps 1.1–3.0 (mevcut çözücü yeniden kullanılır; akış denetimi, ps 1.x doku komutları, türevler), D3D9 TSS zinciri, GL doku ortamı, sis, alfa testi. | Desteklenmeyen yapı → açık hata (arka uç CPU'ya düşer). |
| 1.2 | İki yazıcı: **MSL** (Metal Shading Language fragment fonksiyonu + varying/örnekleyici/sabit yerleşimi) ve **C** (aynı IR'den, Linux'ta test için). | `sgsl_test`: tüm test gölgelendiricileri (tests/unit/shaders + d3d9 testindekiler) ve TSS/GL kombinasyonları için C çıktısı gcc ile derlenip rastgele girdilerde yorumlayıcıyla karşılaştırılır (tolerans 1e-4); MSL çıktısı sözdizimi denetimi için dosyaya yazılır (G3 macOS'ta `xcrun metal` ile derler). |

## G2 — Metal arka ucu (G0'dan sonra; CI'de test)
Dosyalar: yeni `src/sgpu_metal.m` (yalnızca Apple derlemelerinde), `include/SoulCore.h` (`soul_gpu_enable()`), `App/Engine/` (etkinleştirme), `App/Models/AppSettings.swift` + ayarlar ekranı ("GPU hızlandırma (Metal)", varsayılan açık).

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 2.1 | `SgBackend` Metal uygulaması: MTLDevice/kuyruk, render hedefleri (renk + Depth32Float_Stencil8), doku önbelleği (SgTexture sürümüne göre; 2D/küp/hacim, mip'ler), köşe yükleme halka tamponu, boru hattı önbelleği (program özeti + karışım/derinlik/şablon/maske/biçim anahtarı), MSL derleme önbelleği (G1), örnekleyici durumları, makas/görünüm alanı/kırpma düzlemleri, nokta sprite'ları. | macOS CI'de derlenir; G3 düzeneğinde OpenGL ve D3D9 testleri GPU modunda toleransla geçer. |
| 2.2 | CPU'ya eşitleme (paylaşımlı bellek blit'i) ve pencereye `Present`; çevrilemeyen programlarda çizim başına CPU yedeği; Metal açılamazsa CPU arka ucu. | Geri düşme testi CI'de; simülatörde örnekler Metal ile çizilir. |

## G3 — macOS test düzeneği ve CI (G0'dan sonra, G2 ile paralel)
Dosyalar: `Makefile` (macOS hedefi: `make macos` → soulrun + Metal arka ucu), gerekirse küçük taşınabilirlik düzeltmeleri, yeni `tests/gpu/run_gpu_tests.sh` + toleranslı karşılaştırıcı, `.github/workflows/build.yml` (yeni "macOS GPU tests" işi), simülatör testinde ekran görüntüsü karşılaştırması.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 3.1 | soulrun macOS arm64'te derlenir ve çalışır (yorumlayıcı; mümkünse MAP_JIT ile JIT), win32 testleri macOS'ta geçer. | CI'de macOS işi yeşil. |
| 3.2 | `SOUL_GPU=metal` ile d3d9 testi, OpenGL GUI testi ve örnekler; piksel değerleri CPU beklenen çıktısıyla ±2 toleransla karşılaştırılır; G1'in MSL dosyaları `xcrun metal` ile derlenir; CPU ve Metal ile kare/saniye ölçümü yazdırılır. | CI'de geçer; ölçüm raporlanır. |

## G4 — Entegrasyon (G1, G2, G3'ten sonra)
Dosyalar: `README.md`, `App/Views/AboutView.swift`, `App/Models/WinProgram.swift` (D3D9 "yavaş olabilir" notu Metal açıkken kalkar), CI düzenlemeleri.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 4.1 | Belgeler ve uygulama metinleri; ayarlar ekranında GPU durumu (cihaz adı, etkin/yedek). | CI yeşil (Linux + IPA + simülatör + macOS GPU). |
| 4.2 | Performans sonuçları (CI'deki CPU/Metal kare hızları) README'de. | — |

## Denetim (Opus)
Her grup sonrası `git diff` incelemesi ve testler; Metal kısmı için CI günlükleri; sonunda kabul kriterleri tablosu.
Gerçek iPad'deki performansı yalnızca kullanıcı ölçebilir — tur sonunda deneme adımlarını yazarım.
