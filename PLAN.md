# Soul — sonraki tur planı: MSI kurulumları, GDI+, XAudio2

Hedef: `.msi` kurulum paketlerini (Windows Installer) kurup kaldırabilmek, GDI+ kullanan programların
(resim yükleme/çizim, .NET WinForms'un temeli) açılması ve XAudio2 kullanan oyunların ses vermesi.
Kapsam dışı (sonraki tur): LZX/Quantum sıkıştırmalı CAB, MSI betik özel eylemleri (VBScript/JScript),
MSI'nin kendi diyalog tabloları (Dialog/Control), WIC, Direct3D, .NET CLR.

Tüm yollar `SoulCore/` altına göredir (uygulama dosyaları hariç). Derleme: `make` (yerel), `make arm64` (JIT,
qemu-aarch64 ile). Test takımları: `tests/run_tests.sh`, `tests/win32/run_win32_tests.sh`, `tests/gui/run_gui_tests.sh`.

## Gruplar ve sıra

```
G0 (altyapı) ──┬──> G1a (MSI veritabanı + CAB) ──> G1b (MSI kurulum motoru + msiexec) ──┐
               ├──> G2  (GDI+) ─────────────────────────────────────────────────────────┼──> G4 (entegrasyon)
               └──> G3  (XAudio2) ──────────────────────────────────────────────────────┘
```
G1a, G2, G3 paralel çalışır. Her grup yalnızca kendi dosyalarına dokunur.

---

## G0 — Altyapı (önce, tek başına)
Dosyalar: `src/inflate.c`, `src/inflate.h`, `src/registry.c`, `src/hle_com.c`, `src/hle.h`, `src/hle_core.c`,
`src/hle_shell.c`, yeni boş iskeletler `src/msi_install.c`, `src/gdiplus.c`, `src/xaudio2.c`, `tests/unit/`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 0.1 | `inflate.c/.h`: `long soul_inflate(in, in_len, out, out_cap, dict, dict_len)` (ham DEFLATE, isteğe bağlı ön sözlük — MSZIP blokları için) ve `long soul_zlib_inflate(in, in_len, out, out_cap)` (PNG için zlib sarmalayıcı). Hata/taşma = -1. | `tests/unit/inflate_test.c` (yerel gcc) Python `zlib` ile üretilen vektörleri (saklı, sabit, dinamik Huffman, sözlüklü) birebir açar. |
| 0.2 | `registry.c`: ana taraf erişimi — `reg_host_set/get/delete_value/delete_tree/key_exists(root, subkey, name, …)` (UTF-8 dizgeler, REG_SZ/EXPAND_SZ/MULTI_SZ/DWORD/BINARY). `hle.h`'ye bildirimler. | Birim testi ya da küçük bir guest programı: `reg_host_set` ile yazılan değer `RegQueryValueExW` ile okunur; mevcut registry testleri geçer. |
| 0.3 | `hle_com.c`: `int soul_lnk_write(host_path, target_win, args, workdir, desc, icon, icon_index, show)` dışa açılır (mevcut statik `lnk_write` kullanılır). `CoCreateInstance(Ex)` ve `CoGetClassObject` önce `xaudio2_com_create(st, clsid, riid, ppv)`'yi dener. | `soul_lnk_read` ile geri okunan alanlar aynı; multiproc testi geçer. |
| 0.4 | `hle.h`/`hle_core.c`: `hle_msi_table`, `hle_gdiplus_table`, `hle_xaudio2_table` `find_entry`'ye eklenir; `xaudio2_7/8/9`, `xaudio2_9redist`, `x3daudio1_7`, `x3daudio1_9` sistem DLL listesine; `k_extra_stdcall`'a `XAudio2Create`=12 (X3DAudio fonksiyonları cdecl, eklenmez). `hle_shell.c`'deki GDI+ saplamaları silinir. İskelet dosyalar boş tablo ve `xaudio2_com_create` saplaması (`REGDB_E_CLASSNOTREG`) içerir. | `make` ve `make arm64` uyarısız; 52 + 23 + 10 test geçer. |

## G1a — MSI veritabanı ve CAB (G0'dan sonra)
Dosyalar: `src/msi.h`, `src/msi_db.c`, `src/cab.c`, `tests/msi/` (wxs + derleme betiği + hazır `sample.msi`), `tests/unit/msidb_test.c`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 1a.1 | `msi_db.c`: OLE bileşik dosya (CFB v3/v4, FAT/MiniFAT/dizin) okuyucu; MSI akış adı kodlaması (0x3800 tabanlı ad sıkıştırma); `_StringPool`/`_StringData` (kod sayfası, referans sayıları, 3 baytlık indeks bayrağı), `_Tables`/`_Columns` ile tablo şeması; sütun-ana (column-major) satır okuma; `\005SummaryInformation`. | `msidb_test sample.msi` tüm tabloları, satır sayılarını ve özet bilgisini doğru yazdırır (wixl'in girdisiyle karşılaştırılır). |
| 1a.2 | `msi.h` arayüzü: `msidb_open/close`, `msidb_table_exists`, `msidb_rows`, `msidb_columns`, `msidb_str`, `msidb_int` (`MSI_NULL_INT`), `msidb_find`, `msidb_stream`, `msidb_summary`; basit SQL görünümü `msidb_query(db, sql, …)` (`SELECT kolonlar|* FROM t [WHERE a = 'x' AND b = ?]`). | `msidb_test` birkaç SQL sorgusunu doğru satırlarla döndürür. |
| 1a.3 | `cab.c`: `cab_extract(buf, len, cb, ctx, err, errlen)` — CFHEADER/CFFOLDER/CFFILE/CFDATA, sıkıştırmasız ve MSZIP (blok arası 32 KB sözlükle `soul_inflate`); LZX/Quantum anlaşılır hata döndürür. | `sample.msi` içindeki gömülü cab'den çıkan dosyalar kaynak dosyalarla bayt bayt aynı. |

## G1b — MSI kurulum motoru, msi.dll API'si ve msiexec (G1a'dan sonra)
Dosyalar: `src/msi_install.c`, `tools/msiexec/msiexec.c`, `tools/gen_stub.py`, `src/msiexec_stub.h`, `tests/win32/msi.c`, `tests/win32/msi.expected`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 1b.1 | Kurulum motoru: özellikler (ProgramFilesFolder, SystemFolder, AppDataFolder, ProgramMenuFolder, DesktopFolder, TARGETDIR=C:\\, VersionNT, Installed, ALLUSERS, UserLanguageID=1055 …), koşul ifadesi değerlendirici, Directory çözümleme, Feature/Component seçimi, `InstallExecuteSequence` standart eylemleri (CostInitialize…InstallFinalize: CreateFolders, InstallFiles, RemoveFiles, WriteRegistryValues, RemoveRegistryValues, CreateShortcuts, RemoveShortcuts, WriteIniValues, RegisterProduct/PublishProduct → Uninstall anahtarı), özel eylemler 51/35/19 ve 1/17 (DLL, guest'te `soul_guest_call`), 2/18/34/50 (EXE). Paket `C:\Windows\Installer\{ProductCode}.msi` olarak saklanır. | `tests/win32/msi.c` sessiz kurulumda dosyaları, kayıt değerini, kısayolu ve `MsiQueryProductStateW`=5'i doğrular; kaldırmada hepsi silinir. 64/32-bit ve JIT'te aynı çıktı. |
| 1b.2 | msi.dll HLE tablosu: `MsiInstallProductA/W`, `MsiConfigureProduct(Ex)A/W`, `MsiSetInternalUI`, `MsiSetExternalUIA/W`, `MsiOpenPackage(Ex)A/W`, `MsiOpenDatabaseA/W`, `MsiDatabaseOpenViewA/W`, `MsiViewExecute/Fetch/Close`, `MsiRecord*`, `MsiGetProperty/SetPropertyA/W`, `MsiCloseHandle`, `MsiQueryProductStateA/W`, `MsiGetProductInfoA/W`, `MsiEnumProductsA/W`, `MsiGetSummaryInformation*`. Dış UI geri çağrısına ilerleme/eylem mesajları gider. | Test programı özel eylem DLL'inden `MsiGetPropertyW` ile özellik okur; ilerleme mesajları gelir. |
| 1b.3 | `tools/msiexec/msiexec.c`: küçük bir 32-bit Win32 programı (Türkçe arayüz): `/i /x /a /qn /qb /passive PROP=değer` ayrıştırır; hoş geldin (ürün adı, kurulum klasörü), ilerleme çubuğu (dış UI geri çağrısı), bitiş/hata sayfaları. `gen_stub.py` bunu `src/msiexec_stub.h` bayt dizisine çevirir. | `soulrun msiexec.exe /i sample.msi /qn` kurar ve 0 döndürür; GUI betiğiyle *Kur* tıklanınca kurulum biter. |

## G2 — GDI+ (G0'dan sonra, G1a/G3 ile paralel)
Dosyalar: `src/gdiplus.c`, `src/imgcodec.c`, `src/imgcodec.h`, `tests/win32/gdiplus.c`, `tests/win32/gdiplus.expected`, `tests/win32/gdiplus_img/`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 2.1 | `imgcodec.c`: BMP, PNG (tüm renk türleri, interlace, `soul_zlib_inflate`), GIF (ilk kare, saydamlık), JPEG baseline + progressive → ARGB; BMP ve PNG kodlayıcı. | Python/PIL ile üretilen test görüntüleri belirlenen piksellerde doğru renkte çözülür (JPEG için ±8 tolerans). |
| 2.2 | `gdiplus.c` düz API (~120 fonksiyon): Startup/Shutdown, Graphics (FromHDC/FromHWND/FromImage, Clear, dönüşümler, kırpma, smoothing), Pen/SolidBrush/LinearGradientBrush, çizgi/dikdörtgen/elips/yay/çokgen/yol (GraphicsPath) çizme-doldurma (kenar yumuşatma), Image/Bitmap (dosyadan, IStream'den, Scan0, HBITMAP/HICON dönüşümleri, LockBits, Get/SetPixel, Save), DrawImage ailesi (ölçekleme), Font/FontFamily/StringFormat, DrawString/MeasureString (mevcut GDI metin çizici). HDC hedefi: geçici ARGB katmanı + `gdi_draw_argb`. | `tests/win32/gdiplus.c` DIB bölümüne çizer, piksel değerleri beklenen çıktıyla aynı; 64/32-bit ve JIT'te aynı. |

## G3 — XAudio2 (G0'dan sonra, paralel)
Dosyalar: `src/audio.c`, `src/audio.h`, `src/xaudio2.c`, `tests/win32/xaudio2.cpp`, `tests/win32/xaudio2.expected`, `tests/win32/audio_check.py`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 3.1 | `audio.c`'ye iç ses API'si (`audio.h`): kaynak ses oluştur/kuyruğa tampon ekle/başlat/durdur/temizle/frekans oranı/ses düzeyi, tampon bitti bildirimi için geri çağrı kancası. Mevcut waveOut/DirectSound davranışı değişmez. | Mevcut `audio` testleri ve kayıttaki tonlar aynı. |
| 3.2 | `xaudio2.c`: `XAudio2Create` (2.8/2.9) ve `CoCreateInstance(CLSID_XAudio2)` (2.7, farklı vtable düzeni) — IXAudio2, mastering/submix/source voice, `SubmitSourceBuffer` (PCM 8/16/24/32, float; döngü bölgeleri), `GetState`, `IXAudio2VoiceCallback`/`IXAudio2EngineCallback` çağrıları süreç başına bir guest iş parçacığında; X3DAudio basit hesap. | `tests/win32/xaudio2.cpp`: iki tampon çalınır, OnBufferEnd ×2 ve OnStreamEnd gelir, `SamplesPlayed` doğru; `audio_check.py --tones 440,660` kayıtta tonları duyar. 64/32-bit, JIT. |

## G4 — Entegrasyon (G1b, G2, G3'ten sonra)
Dosyalar: `src/hle_core.c`, `include/SoulCore.h`, `src/hle_process.c`, `tests/win32/run_win32_tests.sh`,
`App/Models/ProgramLibrary.swift`, `App/Models/WinProgram.swift`, `App/Models/SoulPaths.swift`, `README.md`.

| # | Değişiklik | Kabul kriteri |
|---|---|---|
| 4.1 | `soul_install_system_files(drive_c)` (SoulCore.h): msiexec stub'ını `C:\Windows\System32\msiexec.exe` olarak yazar; `hle_init_system` de çağırır. `hle_process.c` yerleşik araç listesinden `msiexec` çıkar (gerçek stub çalışır). | Bir guest programının `CreateProcess("msiexec /i x.msi /qn")` çağrısı kurulumu yapar. |
| 4.2 | Uygulama: `.msi` dosyaları kurulum dosyası olarak tanınır; *Kur* `msiexec.exe /i "<msi>"` ile açılır; uygulama açılışında `soul_install_system_files` çağrılır; kurulum sonrası kısayol taraması mevcut akışla. | CI'de uygulama derlenir (Xcode); kod incelemesinde akış doğru. |
| 4.3 | `run_win32_tests.sh`: msi, gdiplus, xaudio2 testleri (64/32-bit) eklenir; README uyumluluk tablosu ve yol haritası güncellenir. | Yerel + qemu JIT: tüm takımlar geçer. |

## Denetim (Opus)
`git diff` incelemesi, tüm test takımları (yerel + JIT), her kabul kriteri için tablo halinde sonuç.
