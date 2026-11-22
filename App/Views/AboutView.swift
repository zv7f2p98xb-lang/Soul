import SwiftUI

struct AboutView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                Text("Soul nasıl çalışır?").font(.largeTitle.weight(.bold))
                Text("Soul, bilgisayarlar (PC) için yazılmış 64-bit (x64) programları — pencereli uygulamaları, oyunları ve konsol araçlarını — iPad'in Apple Silicon (ARM64) işlemcisinde çalıştırır. Bunun için dört katman birlikte çalışır:")
                    .foregroundStyle(.secondary)

                LayerCard(icon: "doc.zipper", color: .blue, title: "1 · PE yükleyici",
                          text: "Windows .exe/.dll dosyalarını (PE32+) belleğe yerleştirir, yer değiştirmeleri uygular, içe aktarılan işlevleri bağlar, TLS ve DllMain çağrılarını yapar. Programın yanındaki DLL'ler de x64 kodu olarak yüklenir.")
                LayerCard(icon: "bolt.fill", color: .green, title: "2 · x86 / x86-64 → ARM64 JIT",
                          text: "64-bit (x86-64) ve 32-bit (x86) makine kodu temel bloklar halinde çözülür ve çalışma anında ARM64 komutlarına çevrilir. Tamsayı işlemleri, bellek erişimleri, dallanmalar, SSE ve x87 kayan nokta işlemleri doğrudan M çipinde çalışır; EFLAGS bayrakları tembel (lazy) hesaplanır ve mümkün olduğunda ARM'ın NZCV bayraklarıyla eşlenir. 32-bit programlar 4 GB'lık ayrı bir bellek penceresinde çalışır. Çevrilemeyen nadir komutlar blok içinden yorumlayıcıya devredilir.")
                LayerCard(icon: "square.stack.3d.up.fill", color: .purple, title: "3 · Win32 uyumluluk katmanı",
                          text: "kernel32, msvcrt, UCRT (api-ms-win-crt-*), vcruntime, user32, gdi32, advapi32 ve shell32'nin yüzlerce işlevi iOS üzerinde yeniden yazıldı: dosyalar (C: sürücüsü uygulamanın Belgeler klasörüne bağlıdır), konsol, bellek, zaman, pencereler, mesaj kuyruğu, GDI çizimi, denetimler, menüler, diyaloglar, MessageBox ve daha fazlası.")
                LayerCard(icon: "cube.transparent.fill", color: .orange, title: "4 · Grafik çeviri katmanı",
                          text: "Programın 3B çizim çağrıları (OpenGL 1.x, WGL, GLU) Soul'un GPU biçimli çizim hattına (sgpu) çevrilir: kırpma, perspektif-doğru doku, derinlik tamponu, ışık, sis ve saydamlık. Kareler pencereye aktarılır; Direct3D ön yüzü ve Metal (GPU) arka ucu aynı yapıya eklenecek.")

                Text("Neler çalışır?").font(.title2.weight(.semibold))
                Bullet(ok: true, "64-bit (x64) ve 32-bit (x86) programlar: ikisi de JIT ile ARM64'e çevrilip M çipinde çalışır")
                Bullet(ok: true, "Konsol programları (C, C++, MinGW veya MSVC ile derlenmiş)")
                Bullet(ok: true, "Dosya okuma/yazma, klasör listeleme, klavye girdisi, renkli konsol çıktısı")
                Bullet(ok: true, "Programla gelen DLL'ler, statik C++ çalışma zamanı, setjmp/longjmp, TLS; 32-bit programlarda C++ istisnaları ve Windows istisna işleme (SEH)")
                Bullet(ok: true, "Pencereli (GUI) programlar: pencereler, GDI çizimi, düğme/metin kutusu/liste denetimleri, menüler, diyaloglar, zamanlayıcılar")
                Bullet(ok: true, "OpenGL 1.x 3B grafik: Soul'un grafik çeviri katmanı (doku, ışık, sis, saydamlık, derinlik tamponu)")
                Bullet(ok: true, "Dokunma → fare (uzun basış = sağ tık, iki parmak = tekerlek), donanım ve ekran klavyesi")
                Bullet(ok: true, "Windows mesaj kutuları (MessageBox) iPad uyarısı olarak gösterilir")
                Bullet(ok: true, "Kalıcı kayıt defteri: programlar ayarlarını kaydeder ve bir sonraki açılışta bulur; bağlantılar Safari'de açılır")
                Bullet(ok: true, "Çoklu iş parçacığı: CreateThread, kritik bölgeler, olaylar, semaforlar, koşul değişkenleri")

                Bullet(ok: true, "Kurulum sihirbazları (Inno Setup, NSIS, Windows Installer .msi), alt süreç başlatma (CreateProcess)")
                Bullet(ok: true, "GDI+ çizimi ve görüntü dosyaları, ses (waveOut, DirectSound, XAudio2)")

                Text("Henüz çalışmayanlar").font(.title2.weight(.semibold))
                Bullet(ok: false, "Direct3D / Vulkan / gölgelendiricili (GLSL) OpenGL kullanan 3B oyunlar, Steam ve web tabanlı başlatıcılar")
                Bullet(ok: false, ".NET programları, Direct3D, LZX ile sıkıştırılmış .msi paketleri")
                Bullet(ok: false, "64-bit programlarda C++ istisnaları (SEH), AVX komutları")

                Text("Madeira ile fark").font(.title2.weight(.semibold))
                Text("Madeira gibi projeler Steam oyunlarını çalıştırmak için Wine (Windows API'nin neredeyse tamamı), FEX-Emu (olgun bir x86→ARM64 JIT) ve DXMT (Direct3D 11 → Metal) yığınını birleştirir. Soul ise sıfırdan yazılmış, küçük ve anlaşılır bir çekirdek: aynı temel fikri (JIT + API taklidi + grafik çeviri katmanı) uygular: konsol, GDI tabanlı pencereli programlar ve OpenGL 1.x 3B programlar çalışır. Yol haritası: Direct3D 9, Metal (GPU) arka ucu, gölgelendirici çevirisi (Direct3D 11), ses ve ağ.")
                    .foregroundStyle(.secondary)
            }
            .padding(24)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .navigationTitle("Nasıl Çalışır?")
    }
}

private struct LayerCard: View {
    let icon: String
    let color: Color
    let title: String
    let text: String
    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundStyle(.white)
                .frame(width: 48, height: 48)
                .background(color.gradient, in: RoundedRectangle(cornerRadius: 12))
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.headline)
                Text(text).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
    }
}

private struct Bullet: View {
    let ok: Bool
    let text: String
    init(ok: Bool, _ text: String) { self.ok = ok; self.text = text }
    var body: some View {
        Label {
            Text(text)
        } icon: {
            Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill").foregroundStyle(ok ? .green : .red)
        }
    }
}
