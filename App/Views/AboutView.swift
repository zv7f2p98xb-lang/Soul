import SwiftUI

struct AboutView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                Text("Soul nasıl çalışır?").font(.largeTitle.weight(.bold))
                Text("Soul, Windows için derlenmiş 64-bit (x64) programları iPad'in Apple Silicon (ARM64) işlemcisinde çalıştırır. Bunun için üç katman birlikte çalışır:")
                    .foregroundStyle(.secondary)

                LayerCard(icon: "doc.zipper", color: .blue, title: "1 · PE yükleyici",
                          text: "Windows .exe/.dll dosyalarını (PE32+) belleğe yerleştirir, yer değiştirmeleri uygular, içe aktarılan işlevleri bağlar, TLS ve DllMain çağrılarını yapar. Programın yanındaki DLL'ler de x64 kodu olarak yüklenir.")
                LayerCard(icon: "bolt.fill", color: .green, title: "2 · x86-64 → ARM64 JIT",
                          text: "x86-64 makine kodu temel bloklar halinde çözülür ve çalışma anında ARM64 komutlarına çevrilir. Tamsayı işlemleri, bellek erişimleri, dallanmalar ve kayan nokta işlemleri doğrudan M çipinde çalışır; EFLAGS bayrakları tembel (lazy) hesaplanır ve mümkün olduğunda ARM'ın NZCV bayraklarıyla eşlenir. Çevrilemeyen nadir komutlar (ör. x87, bazı SSE) blok içinden yorumlayıcıya devredilir.")
                LayerCard(icon: "square.stack.3d.up.fill", color: .purple, title: "3 · Win32 uyumluluk katmanı",
                          text: "kernel32, msvcrt, UCRT (api-ms-win-crt-*), vcruntime, user32, advapi32 ve shell32'nin yüzlerce işlevi iOS üzerinde yeniden yazıldı: dosyalar (C: sürücüsü uygulamanın Belgeler klasörüne bağlıdır), konsol, bellek, zaman, ortam değişkenleri, MessageBox ve daha fazlası.")

                Text("Neler çalışır?").font(.title2.weight(.semibold))
                Bullet(ok: true, "64-bit konsol programları (C, C++, MinGW veya MSVC ile derlenmiş)")
                Bullet(ok: true, "Dosya okuma/yazma, klasör listeleme, klavye girdisi, renkli konsol çıktısı")
                Bullet(ok: true, "Programla gelen x64 DLL'ler, statik C++ çalışma zamanı, setjmp/longjmp, TLS")
                Bullet(ok: true, "Windows mesaj kutuları (MessageBox) iPad uyarısı olarak gösterilir")
                Bullet(ok: true, "Çoklu iş parçacığı: CreateThread, kritik bölgeler, olaylar, semaforlar, koşul değişkenleri")

                Text("Henüz çalışmayanlar").font(.title2.weight(.semibold))
                Bullet(ok: false, "Pencereli (GUI) programlar ve oyunlar: GDI/Direct3D/OpenGL çizimi yok")
                Bullet(ok: false, "32-bit (x86) programlar, .NET programları, kurulum sihirbazları (.msi)")
                Bullet(ok: false, "C++ istisnaları (throw), AVX komutları, alt süreç başlatma (CreateProcess)")

                Text("Madeira ile fark").font(.title2.weight(.semibold))
                Text("Madeira gibi projeler Steam oyunlarını çalıştırmak için Wine (Windows API'nin neredeyse tamamı), FEX-Emu (olgun bir x86→ARM64 JIT) ve DXMT (Direct3D 11 → Metal) yığınını birleştirir. Soul ise sıfırdan yazılmış, küçük ve anlaşılır bir çekirdek: aynı temel fikri (JIT + API taklidi) uygular ama şimdilik konsol programlarını hedefler. Yol haritası: GDI pencereleri, dokunmatik/klavye girdisi, ardından Direct3D → Metal.")
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
