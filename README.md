# Soul — iPad'de bilgisayar (PC) programları

Soul, bilgisayarlar (Windows PC) için derlenmiş **64-bit (x64) ve 32-bit (x86) programları** Apple Silicon (M serisi) iPad'lerde — örneğin **iPad Air M3** — çalıştıran bir iPadOS uygulamasıdır. Programın x86 / x86-64 makine kodu çalışma anında **ARM64 koduna çevrilir (JIT)** ve doğrudan M çipinde çalışır; Windows API çağrıları ise iOS üzerinde yeniden yazılmış bir uyumluluk katmanı tarafından karşılanır.

> Durum: **konsol ve pencereli (GUI) programlar çalışıyor** — C/C++ (MinGW, MSVC/UCRT) programları; dosya, klavye, renkli konsol, çoklu iş parçacığı, **Win32 pencereleri, GDI çizimi, düğme/metin kutusu/liste denetimleri, menüler ve diyaloglar** ve **OpenGL 1.x ile 3B grafik** (Soul'un kendi grafik çeviri katmanı). **32-bit (x86) programlar da çalışıyor** — konsol, pencereli ve OpenGL, JIT dahil (x87 FPU komutları da ARM64'e çevrilir). Direct3D oyunları ve kurulum sihirbazları **henüz yok** — ayrıntılar aşağıda.
>
> Ölçüm (CI'da Apple Silicon üzerinde iOS simülatörü, JIT açık): JIT Benchmark örneği **329 milyon x86 komutunu 0,18 sn'de** çalıştırdı (~1,8 milyar komut/sn), yorumlayıcıya göre ~20 kat hızlı.

## Mimari

```
 .exe (PE32+ x86-64 veya PE32 x86)
   │
   ▼
┌──────────────────────────┐   ┌───────────────────────────────────────────┐
│ PE yükleyici (pe.c)      │──▶│ Win32 / CRT katmanı (hle_*.c)              │
│ relocation, import, TLS, │   │ kernel32, msvcrt, UCRT (api-ms-win-crt-*), │
│ DllMain, kaynaklar,      │   │ vcruntime, advapi32, shell32, ...          │
│ x64 / x86 DLL'ler        │   │ user32.c / controls.c / gdi.c: pencereler, │
└──────────────────────────┘   │ mesajlar, denetimler, diyaloglar, GDI      │
                               └───────────────────────────────────────────┘
   │                                      ▲  int3 "SC" köprüsü (thunk)
   ▼                                      │
┌──────────────────────────────────────────────────────────────────────────┐
│ x86 / x86-64 çekirdeği (64-bit ve 32-bit kip)                             │
│  decode.c   : komut çözücü (REX, ModRM/SIB, SSE önekleri, 32-bit kip)     │
│  jit_a64.c  : temel blok → ARM64 çevirmen (NZCV ↔ EFLAGS eşleme,          │
│               tembel bayraklar, blok içi yorumlayıcı çağrıları)           │
│  interp.c / sse.c / x87.c : tam yorumlayıcı (tamsayı, SSE2, x87)          │
│  jit_mem.c  : iOS JIT belleği (klasik RW/RX ayna + iOS 26 TXM)            │
└──────────────────────────────────────────────────────────────────────────┘
   │
   ▼
 SwiftUI iPad uygulaması (App/): kitaplık, kurulum, konsol, pencere masaüstü, JIT durumu
```

- **Kimlik eşlemeli bellek:** 64-bit programlarda misafir adresleri doğrudan ana süreç adresleridir (Box64/FEX'teki gibi), bu yüzden bellek erişimleri JIT kodunda tek bir ARM64 `ldr/str` olur.
- **32-bit programlar (gwin.c):** iOS her sürecin alttaki 4 GB'ını boş bıraktığından, 32-bit program için 4 GB'lık bir pencere ayrılır; misafir adresi `a`, ana süreçte `taban + a`'dır. JIT tabanı `x20` yazmacında tutar, bellek erişimi `add x6, x20, w6 (uxtw)` + `ldr/str` olur. Programın görüp kullanabileceği her şey (imajlar, yığınlar, TEB/PEB, heap, API'nin döndürdüğü tamponlar, pencere yordamına giden yapılar) bu pencerede ayrılır. API köprüsü argümanları yığından okur, stdcall fonksiyonlarının yığın temizliğini i686 içe aktarım kitaplıklarından üretilen bir tablodan (13 bin fonksiyon) yapar; 32/64-bit yapı düzenleri (`MSG`, `WNDCLASS`, `CREATESTRUCT`, `OPENFILENAME`…) tek bir düzen tanımından iki kip için okunur/yazılır. x87 FPU komutları (yığın modeli dahil) JIT'te doğrudan ARM64 kayan nokta komutlarına çevrilir.
- **Bayraklar:** `cmp/sub/add/test/and/or/xor/inc/dec/shift` ARM'ın `subs/adds/ands` komutlarıyla çevrilir; aynı bloktaki `jcc/cmovcc/setcc` doğrudan NZCV koşullarını kullanır (x86 CF ile ARM C'nin ters anlamı dikkate alınır). Blok dışına taşan bayraklar tembel olarak (`lf_*`) saklanır.
- **Callout:** JIT'in doğal olarak çeviremediği her kontrol-akışı-dışı komut (nadir x87/SSE komutları, BCD…) blok terk edilmeden yorumlayıcıya devredilir. Paketlenmiş SSE2 aritmetiği NEON'a, DIV/IDIV `sdiv/udiv/msub`'a çevrilir.
- **Blok zincirleme:** Blok çıkışları hızlı arama tablosunu satır içinde yoklayıp doğrudan sonraki çevrilmiş bloğa atlar; C dağıtıcısına dönülmez.
- **İş parçacıkları:** Her misafir iş parçacığı kendi x86 durumu, TEB'i, yığını ve TLS kopyasıyla ayrı bir ana iş parçacığında çalışır. Misafir kodu küresel bir kilit (GIL) altında yürür; bir zamanlayıcı her ~4 ms'de zaman dilimi verir, engelleyici API'ler (Sleep, Wait*, kritik bölge, SRW, koşul değişkeni, konsol girdisi, MessageBox) kilidi bırakır.
- **Pencereler (GUI):** Her üst düzey pencerenin istemci alanı için 32-bit bir çerçeve arabelleği tutulur; alt pencereler (denetimler) üst pencerenin arabelleğine kendi konumlarında çizilir. Pencere başlığı, kapatma düğmesi ve menü çubuğu iPad tarafında (SwiftUI) çizilir; programın menüleri yerel iPad menüleri olarak açılır. Mesaj kuyruğu, `WM_PAINT`/geçersiz bölge mantığı, zamanlayıcılar, odak/etkinleştirme, `DefWindowProc`, alt sınıflama (`SetWindowLongPtr`), ANSI(UTF-8)/Unicode dönüşümleri ve modal diyalog döngüleri Win32'deki gibi çalışır. GDI; kalem/fırça, şekiller, DIB bölümleri, `BitBlt/StretchBlt/AlphaBlend`, bölge kırpma ve gömülü DejaVu yazı tipleriyle kenar yumuşatmalı metin çizer. Hazır denetimler: Button (düğme, onay kutusu, radyo, grup), Static, Edit (tek/çok satır, seçim, pano), ListBox, ComboBox, ilerleme çubuğu, durum çubuğu, kaydırıcı (trackbar), RichEdit (düz metin olarak); henüz yazılmamış ortak denetimler (liste/ağaç görünümü vb.) boş bir panel olarak oluşturulur, böylece program açılmaya devam eder; kaynaklardan diyalog, menü, dizge, bit eşlem ve kısayol tabloları yüklenir. `GetOpenFileName`/`GetSaveFileName` (Dosya Aç/Kaydet), Soul'un kendi denetimleriyle çizilen ve C: sürücüsünde gezinen bir diyalogdur.
- **iOS JIT:** StikDebug, Soul başladıktan *sonra* bağlandığı için Soul `CS_DEBUGGED` bayrağını sürekli yoklar ve bayrak görünür görünmez JIT belleğini arka planda hazırlar. **iPadOS 26+ (TXM)** cihazlarda StikDebug'ın `universal.js` betiğinin anladığı `brk #0xf00d` / `x16 = 1` (JIT26PrepareRegion) çağrısıyla RX bölge istenir ve `vm_remap` + `vm_protect` ile RW aynası oluşturulur; çalıştırma izni ayıklayıcıya bağlı olabileceğinden ayıklayıcı ayrılmaz. Olmazsa klasik yönteme (RW/RX ayna) geçilir. Her iki yolda da bellek, küçük bir deneme fonksiyonu yazılıp çalıştırılarak doğrulanır; kurulumdaki tüm hatalar yakalanır, böylece bozuk bir JIT asla kullanılmaz ve uygulama çökmez. Adımlar *JIT Durumu → JIT günlüğü* bölümünde görünür.

## iPad Air M3'e kurulum

1. **IPA'yı alın:** GitHub Actions → *Build* iş akışı → `Soul-ipa` çıktısı (ya da `v*` etiketli sürümler). Kendiniz derlemek için aşağıya bakın.
2. **Yan yükleyin:** SideStore, AltStore veya Sideloadly ile kendi Apple kimliğinizle imzalayıp yükleyin (geliştirici imzası `get-task-allow` ekler — JIT için şart).
3. **StikDebug'ı kurun:** eşleştirme (pairing) dosyasını içe aktarın, LocalDevVPN'i açın.
4. **iPadOS 26 (TXM):** StikDebug ayarlarında **TXM seçeneğini (TXM Override)** açın, *Always Run Scripts* ve *Picture in Picture*'ı da açın, *Scripts* bölümünde **Soul için `universal.js`** seçin. iPad Air M3'te TXM seçeneği açılmadan betik çalışmaz ve Soul JIT'i göremez (kullanıcı testiyle doğrulandı).
5. Soul'da **JIT Durumu → "StikDebug ile JIT'i etkinleştir"** düğmesine basın (StikDebug'ı Soul + `universal.js` ile açar) ya da StikDebug'da **Enable JIT → Soul** seçin, sonra Soul'a geri dönün. Birkaç saniye içinde "JIT etkin" görünmelidir; görünmezse *Tanılama bilgisini kopyala* ile günlüğü paylaşın. JIT olmadan da çalışır ama yorumlayıcı modunda (yavaş).

## Kullanım

- **Program ekle (+):** bir `.exe` ya da program klasörü (DLL'leri/verileriyle) seçin, Dosyalar'dan sürükleyip bırakın ya da Dosyalar'da `.exe`'ye "Soul ile aç" deyin. Program `C:\Program Files\<Ad>\` altına kurulur.
- **C: sürücüsü:** Dosyalar › iPad'imde › Soul › Soul › `drive_c`. Programların yazdığı dosyalar buradadır; buraya kopyaladığınız programları *C: sürücüsünü tara* ile kitaplığa ekleyebilirsiniz.
- **Pencereli programlar:** program pencereleri bir masaüstünde gösterilir ve ekrana sığacak şekilde ölçeklenir. Dokunma sol tıktır (sürükleme = basılı tutup çekme), **uzun basış sağ tıktır**, iki parmakla kaydırma fare tekerleğidir; araç çubuğundaki *Sağ tık* modu tüm dokunuşları sağ tıka çevirir. iPad'e bağlı fare/izleme dörtgeni ve donanım klavyesi doğrudan çalışır (⌘ kısayolları Ctrl olarak gönderilir). Metin kutusuna dokununca ekran klavyesi açılır; *Klavye* düğmesiyle istendiğinde de açılabilir. Yeniden boyutlandırılabilir pencerelerde ⤢ düğmesi pencereyi ekran boyutuna getirir. Programın `printf` çıktısı *Konsol çıktısı* sayfasındadır.
- **Konsol:** çıktı renkli gösterilir, alt satırdan programa klavye girdisi gönderilir; *Ctrl+Z* girdi sonudur. Üst çubukta çalışma süresi, MIPS, JIT oranı ve API çağrısı sayısı görünür.
- **Örnekler:** 3B — Küp 3B (OpenGL); pencereli — Çizim, Saat, Hesap Makinesi (GUI), Not Defteri, Yılan, Mayın Tarlası; konsol — Merhaba, Hesap Makinesi, Sayı Tahmin, Mandelbrot, JIT Benchmark, Sistem Bilgisi, İş Parçacıkları, C++ Demo.

## Uyumluluk

| Çalışır | Henüz çalışmaz |
|---|---|
| 64-bit (x64) ve 32-bit (x86) programlar, ikisi de JIT ile (MinGW, MSVC/UCRT) | Direct3D 9/11/12, Vulkan, OpenGL 2+ gölgelendiricileri (GLSL) |
| OpenGL 1.x 3B grafik (WGL, sabit işlevli çizim hattı, doku, ışık, sis, saydamlık) | 3B grafiğin Metal (GPU) arka ucu — şimdilik CPU'da çizilir |
| Pencereli programlar: user32 pencereleri, GDI, denetimler, menüler, diyaloglar, Dosya Aç/Kaydet | Ortak denetimlerin çoğu (ağaç/liste görünümü, sekmeler, araç çubukları) |
| Dosya/klasör API'leri, ortam değişkenleri, zaman | .NET programları, `.msi` kurulumları |
| printf/scanf ailesi, C++ STL (statik libstdc++) | Alt süreç başlatma (`CreateProcess`, `system`) |
| setjmp/longjmp, TLS, programla gelen x64/x86 DLL'ler; 32-bit programlarda C++ istisnaları ve SEH (fs:[0] zinciri, RaiseException, RtlUnwind, sıfıra bölme yakalama — Delphi/MSVC tarzı) | 64-bit programlarda C++ istisnaları (SEH unwind tabloları), AVX |
| MessageBox, renkli konsol (SetConsoleTextAttribute) | Ağ (Winsock), ses |
| Kalıcı kayıt defteri (programların ayarları korunur), web bağlantıları Safari'de açılır | Kurulum sihirbazları (Inno Setup/NSIS alt süreç başlatır), başka program başlatma |
| CreateThread, kritik bölge, SRW, koşul değişkeni, olay/mutex/semafor | Gerçek paralel çalışma (iş parçacıkları sırayla çalışır) |

Madeira gibi projeler Steam oyunları için Wine + FEX-Emu + DXMT (Direct3D 11 → Metal) yığınını kullanır; Soul aynı temel fikri (JIT + API taklidi + grafik çeviri katmanı) sıfırdan, küçük bir çekirdekle uygular.

### Grafik çeviri katmanı

```
 OpenGL 1.x (opengl32/glu32/WGL)   Direct3D 9 / 11 (sırada)
              │                              │
              ▼                              ▼
 ┌──────────────────────────────────────────────────────────┐
 │ sgpu: GPU biçimli sabit işlevli çizim hattı (sgpu.h)     │
 │ homojen uzayda kırpma, perspektif-doğru enterpolasyon,   │
 │ derinlik/alfa testi, mipmap'li doku, sis, karıştırma     │
 └──────────────────────────────────────────────────────────┘
              │ CPU arka ucu (bugün)        │ Metal arka ucu (sırada)
              ▼                              ▼
     pencere çerçeve arabelleği       iPad GPU'su (Apple Silicon)
```

OpenGL ön yüzü (`opengl.c`) matris yığınlarını, `glBegin/glEnd` ve köşe dizilerini, köşe başına ışıklandırmayı, sisi, çoklu dokuyu, görüntüleme listelerini, `glPushAttrib`'i ve GLU (perspektif, `gluLookAt`, küre/silindir/disk, mipmap) işlevlerini uygular; `SwapBuffers` kareyi pencereye aktarır ve ekran yenileme hızına (60 FPS) göre bekler. Çizim hattı, ön yüzlerden bağımsız bir arayüz (`sg_triangles/sg_lines/sg_points`) sunar; bu sayede Direct3D ön yüzü ve Metal (GPU) arka ucu aynı yapıya eklenebilir. Madeira'nın kullandığı DXMT ölçeğine (Direct3D 11 + gölgelendirici derleyicisi → Metal) ulaşmak çok aşamalı bir yol: sıradaki adımlar Direct3D 9 sabit işlevli ön yüz, Metal arka ucu ve gölgelendirici (DXBC/GLSL) çevirisi.

Yol haritası: Direct3D 9 → Metal arka ucu → gölgelendirici çevirisi (Direct3D 11), ses (waveOut/XAudio2), ağ, ortak denetimler.

## Geliştirme

```bash
# Çekirdek (Linux/macOS): yorumlayıcı + testler
make -C SoulCore
SoulCore/tests/run_tests.sh                 # yerel derlemeyle karşılaştırmalı testler, x64 ve x86 (32-bit) derlemeleriyle (MinGW gerekir)

# ARM64 JIT'i Linux'ta qemu ile test etmek
make -C SoulCore arm64
SoulCore/tests/run_tests.sh qemu-aarch64 build/arm64/soulrun
SoulCore/tests/win32/run_win32_tests.sh     # Win32 API, C++, iş parçacığı, x87 testleri (64 ve 32-bit)
SoulCore/tests/gui/run_gui_tests.sh         # pencere/denetim/diyalog/OpenGL testleri, 64 ve 32-bit (betikle tıklama, yazma, menü)

# Pencereli bir programı betikle sürmek ve ekran görüntüsü almak
SoulCore/build/native/soulrun --gui-script betik.txt program.exe   # komutlar: waitwin, click, drag, key, text, menu, dump, pixel ...

# Bir .exe'yi komut satırından çalıştırmak
SoulCore/build/native/soulrun --stats program.exe arg1 arg2   # --no-jit, -v, -vv, --profile

# iPad uygulaması (macOS + Xcode 16)
brew install xcodegen && xcodegen generate && open Soul.xcodeproj
```

Klasör yapısı:

- `SoulCore/` — C çekirdeği (`include/SoulCore.h` genel API), `tools/soulrun.c`, `tests/`, `samples/`
- `App/` — SwiftUI iPad uygulaması (kitaplık, konsol, JIT durumu, ayarlar)
- `project.yml` — XcodeGen proje tanımı; `.github/workflows/build.yml` — testler, IPA derlemesi ve iPad simülatöründe gerçek Apple Silicon JIT ile örnek programların çalıştırıldığı duman testi
