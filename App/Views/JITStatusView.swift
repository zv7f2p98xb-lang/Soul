import SwiftUI

struct JITStatusView: View {
    @EnvironmentObject private var jit: JITManager
    @EnvironmentObject private var settings: AppSettings

    var body: some View {
        List {
            Section {
                HStack(spacing: 16) {
                    Image(systemName: jit.isAvailable ? "bolt.circle.fill" : "bolt.slash.circle.fill")
                        .font(.system(size: 54))
                        .foregroundStyle(jit.isAvailable ? .green : .orange)
                    VStack(alignment: .leading, spacing: 6) {
                        Text(jit.isAvailable ? "JIT etkin" : "JIT etkin değil").font(.title2.weight(.bold))
                        Text(jit.isAvailable
                             ? "x86-64 kodu çalışma anında ARM64 koduna çevrilip doğrudan M serisi çip üzerinde çalıştırılıyor."
                             : "Programlar şu an yorumlayıcı ile çalışır. JIT için Soul bir hata ayıklayıcı (StikDebug) tarafından başlatılmalı.")
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 8)
                if jit.recoveredFromCrash {
                    Label("Önceki açılışta JIT hazırlığı sırasında uygulama kapandı; güvenli olması için Klasik yönteme geçildi.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
            }

            Section("Durum") {
                LabeledContent("Hata ayıklayıcı bayrağı (CS_DEBUGGED)", value: jit.isDebugged ? "Var" : "Yok")
                LabeledContent("Yöntem", value: jit.method)
                LabeledContent("Seçili mod", value: settings.jitMethod.title)
                LabeledContent("iPadOS", value: jit.osVersion)
                LabeledContent("Cihaz", value: jit.deviceModel)
                LabeledContent("Ana işlemci ARM64", value: soul_host_is_arm64() != 0 ? "Evet" : "Hayır")
                if !jit.lastMessage.isEmpty {
                    Text(jit.lastMessage).font(.footnote).foregroundStyle(.secondary)
                }
            }

            Section {
                Button { jit.openStikDebug() } label: { Label("StikDebug'ı aç", systemImage: "arrow.up.forward.app") }
                Button {
                    jit.refresh()
                    if jit.isDebugged && !jit.isAvailable { jit.initialize() }
                } label: { Label("Durumu yenile / JIT'i yeniden dene", systemImage: "arrow.clockwise") }
            }

            Section("JIT nasıl açılır? (iPad Air M3 dahil)") {
                Step(n: 1, title: "Soul'u yan yükleyin", text: "Soul.ipa dosyasını SideStore, AltStore ya da Sideloadly ile kendi Apple kimliğinizle imzalayıp yükleyin. Geliştirici imzası 'get-task-allow' yetkisini ekler; JIT için bu gereklidir.")
                Step(n: 2, title: "StikDebug'ı kurun", text: "StikDebug'ı yükleyin, eşleştirme (pairing) dosyasını içe aktarın ve LocalDevVPN'i açın.")
                Step(n: 3, title: "iPadOS 26 ve üzeri (TXM)", text: "M3 çipli iPad'ler TXM kullanır. StikDebug ayarlarında 'Always Run Scripts' ve 'Picture in Picture' seçeneklerini açın; Scripts bölümünde Soul için universal.js betiğini atayın. Soul, açılışta brk #0xf00d ile JIT bölgesi ister.")
                Step(n: 4, title: "iPadOS 17–18", text: "Ek betik gerekmez: StikDebug'da 'Enable JIT' ile Soul'u seçmeniz yeterlidir (Klasik yöntem).")
                Step(n: 5, title: "Soul'u StikDebug'dan başlatın", text: "StikDebug → Enable JIT → Soul. Uygulama açıldığında bu ekranda 'JIT etkin' görünmelidir.")
            }
        }
        .navigationTitle("JIT Durumu")
        .onAppear { jit.refresh() }
    }
}

private struct Step: View {
    let n: Int
    let title: String
    let text: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text("\(n)")
                .font(.headline)
                .frame(width: 28, height: 28)
                .background(Color.accentColor.opacity(0.15), in: Circle())
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(text).font(.subheadline).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }
}
