# Soul — iPad'de Windows programları (x86-64 → ARM64 JIT)

Soul, Windows için derlenmiş **64-bit (x64) programları** Apple Silicon (M serisi) iPad'lerde — örneğin **iPad Air M3** — çalıştıran bir iPadOS uygulamasıdır. Programın x86-64 makine kodu çalışma anında **ARM64 koduna çevrilir (JIT)** ve doğrudan M çipinde çalışır; Windows API çağrıları ise iOS üzerinde yeniden yazılmış bir uyumluluk katmanı tarafından karşılanır.

> Durum: **konsol programları çalışıyor** (C, C++, MinGW/MSVC derlemeleri; dosya, klavye, renkli konsol, MessageBox, **çoklu iş parçacığı**). Pencereli (GUI) programlar, DirectX oyunları ve 32-bit programlar **henüz yok** — ayrıntılar aşağıda.
>
> Ölçüm (CI'da Apple Silicon üzerinde iOS simülatörü, JIT açık): JIT Benchmark örneği **329 milyon x86 komutunu 0,18 sn'de** çalıştırdı (~1,8 milyar komut/sn), yorumlayıcıya göre ~20 kat hızlı.

## Mimari

```
 .exe (PE32+, x86-64)
   │
   ▼
┌──────────────────────────┐   ┌───────────────────────────────────────────┐
│ PE yükleyici (pe.c)      │──▶│ Win32 / CRT katmanı (hle_*.c)              │
│ relocation, import, TLS, │   │ kernel32, msvcrt, UCRT (api-ms-win-crt-*), │
│ DllMain, x64 DLL'ler     │   │ vcruntime, user32, advapi32, shell32, ...  │
└──────────────────────────┘   └───────────────────────────────────────────┘
   │                                      ▲  int3 "SC" köprüsü (thunk)
   ▼                                      │
┌──────────────────────────────────────────────────────────────────────────┐
│ x86-64 çekirdeği                                                          │
│  decode.c   : x86-64 komut çözücü (REX, ModRM/SIB, SSE önekleri)          │
│  jit_a64.c  : temel blok → ARM64 çevirmen (NZCV ↔ EFLAGS eşleme,          │
│               tembel bayraklar, blok içi yorumlayıcı çağrıları)           │
│  interp.c / sse.c / x87.c : tam yorumlayıcı (tamsayı, SSE2, x87)          │
│  jit_mem.c  : iOS JIT belleği (klasik RW/RX ayna + iOS 26 TXM)            │
└──────────────────────────────────────────────────────────────────────────┘
   │
   ▼
 SwiftUI iPad uygulaması (App/): kitaplık, kurulum, konsol, JIT durumu
```

- **Kimlik eşlemeli bellek:** Misafir adresleri doğrudan ana süreç adresleridir (Box64/FEX'teki gibi), bu yüzden bellek erişimleri JIT kodunda tek bir ARM64 `ldr/str` olur.
- **Bayraklar:** `cmp/sub/add/test/and/or/xor/inc/dec/shift` ARM'ın `subs/adds/ands` komutlarıyla çevrilir; aynı bloktaki `jcc/cmovcc/setcc` doğrudan NZCV koşullarını kullanır (x86 CF ile ARM C'nin ters anlamı dikkate alınır). Blok dışına taşan bayraklar tembel olarak (`lf_*`) saklanır.
- **Callout:** JIT'in doğal olarak çeviremediği her kontrol-akışı-dışı komut (x87, nadir SSE…) blok terk edilmeden yorumlayıcıya devredilir. Paketlenmiş SSE2 aritmetiği NEON'a, DIV/IDIV `sdiv/udiv/msub`'a çevrilir.
- **Blok zincirleme:** Blok çıkışları hızlı arama tablosunu satır içinde yoklayıp doğrudan sonraki çevrilmiş bloğa atlar; C dağıtıcısına dönülmez.
- **İş parçacıkları:** Her misafir iş parçacığı kendi x86 durumu, TEB'i, yığını ve TLS kopyasıyla ayrı bir ana iş parçacığında çalışır. Misafir kodu küresel bir kilit (GIL) altında yürür; bir zamanlayıcı her ~4 ms'de zaman dilimi verir, engelleyici API'ler (Sleep, Wait*, kritik bölge, SRW, koşul değişkeni, konsol girdisi, MessageBox) kilidi bırakır.
- **iOS JIT:** iPadOS 17–18'de hata ayıklayıcı `CS_DEBUGGED` koyduktan sonra RW/RX ayna eşleme kullanılır. **iPadOS 26 + M3 (TXM)** cihazlarda Soul, StikDebug'ın `universal.js` betiğinin anladığı `brk #0xf00d` / `x16 = 1` (JIT26PrepareRegion) çağrısıyla RX bölge ister, `vm_remap` ile RW aynasını oluşturur ve `x16 = 0` ile hata ayıklayıcıyı bırakır. Betik yoksa SIGTRAP yakalanır, uygulama çökmez.

## iPad Air M3'e kurulum

1. **IPA'yı alın:** GitHub Actions → *Build* iş akışı → `Soul-ipa` çıktısı (ya da `v*` etiketli sürümler). Kendiniz derlemek için aşağıya bakın.
2. **Yan yükleyin:** SideStore, AltStore veya Sideloadly ile kendi Apple kimliğinizle imzalayıp yükleyin (geliştirici imzası `get-task-allow` ekler — JIT için şart).
3. **StikDebug'ı kurun:** eşleştirme (pairing) dosyasını içe aktarın, LocalDevVPN'i açın.
4. **iPadOS 26 (TXM):** StikDebug ayarlarında *Always Run Scripts* ve *Picture in Picture*'ı açın, *Scripts* bölümünde **Soul için `universal.js`** atayın.
5. **StikDebug → Enable JIT → Soul.** Soul'da *JIT Durumu* ekranı "JIT etkin" göstermelidir. JIT olmadan da çalışır ama yorumlayıcı modunda (yavaş).

## Kullanım

- **Program ekle (+):** bir `.exe` ya da program klasörü (DLL'leri/verileriyle) seçin, Dosyalar'dan sürükleyip bırakın ya da Dosyalar'da `.exe`'ye "Soul ile aç" deyin. Program `C:\Program Files\<Ad>\` altına kurulur.
- **C: sürücüsü:** Dosyalar › iPad'imde › Soul › Soul › `drive_c`. Programların yazdığı dosyalar buradadır; buraya kopyaladığınız programları *C: sürücüsünü tara* ile kitaplığa ekleyebilirsiniz.
- **Konsol:** çıktı renkli gösterilir, alt satırdan programa klavye girdisi gönderilir; *Ctrl+Z* girdi sonudur. Üst çubukta çalışma süresi, MIPS, JIT oranı ve API çağrısı sayısı görünür.
- **Örnekler:** Merhaba, Hesap Makinesi, Sayı Tahmin, Mandelbrot, JIT Benchmark, Sistem Bilgisi, İş Parçacıkları, C++ Demo.

## Uyumluluk

| Çalışır | Henüz çalışmaz |
|---|---|
| 64-bit konsol programları (MinGW, MSVC/UCRT) | GUI pencereleri (GDI/USER), DirectX/OpenGL oyunları |
| Dosya/klasör API'leri, ortam değişkenleri, zaman | 32-bit (x86) ve .NET programları, `.msi` kurulumları |
| printf/scanf ailesi, C++ STL (statik libstdc++) | Alt süreç başlatma (`CreateProcess`, `system`) |
| setjmp/longjmp, TLS, programla gelen x64 DLL'ler | C++ istisnaları (`throw`), SEH, AVX |
| MessageBox, renkli konsol (SetConsoleTextAttribute) | Ağ (Winsock), kayıt defteri yazma, ses |
| CreateThread, kritik bölge, SRW, koşul değişkeni, olay/mutex/semafor | Gerçek paralel çalışma (iş parçacıkları sırayla çalışır) |

Madeira gibi projeler Steam oyunları için Wine + FEX-Emu + DXMT (Direct3D 11 → Metal) yığınını kullanır; Soul aynı temel fikri (JIT + API taklidi) sıfırdan, küçük bir çekirdekle uygular. Yol haritası: GDI pencereleri (SwiftUI/Metal yüzeyine çizim) → dokunmatik/klavye girdisi → Direct3D → Metal.

## Geliştirme

```bash
# Çekirdek (Linux/macOS): yorumlayıcı + testler
make -C SoulCore
SoulCore/tests/run_tests.sh                 # yerel derlemeyle karşılaştırmalı testler (MinGW gerekir)

# ARM64 JIT'i Linux'ta qemu ile test etmek
make -C SoulCore arm64
SoulCore/tests/run_tests.sh qemu-aarch64 build/arm64/soulrun
SoulCore/tests/win32/run_win32_tests.sh     # Win32 API, C++ ve iş parçacığı testleri

# Bir .exe'yi komut satırından çalıştırmak
SoulCore/build/native/soulrun --stats program.exe arg1 arg2   # --no-jit, -v, -vv, --profile

# iPad uygulaması (macOS + Xcode 16)
brew install xcodegen && xcodegen generate && open Soul.xcodeproj
```

Klasör yapısı:

- `SoulCore/` — C çekirdeği (`include/SoulCore.h` genel API), `tools/soulrun.c`, `tests/`, `samples/`
- `App/` — SwiftUI iPad uygulaması (kitaplık, konsol, JIT durumu, ayarlar)
- `project.yml` — XcodeGen proje tanımı; `.github/workflows/build.yml` — testler, IPA derlemesi ve iPad simülatöründe gerçek Apple Silicon JIT ile örnek programların çalıştırıldığı duman testi
