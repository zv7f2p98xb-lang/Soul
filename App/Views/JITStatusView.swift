import SwiftUI
import UIKit

struct JITStatusView: View {
    @EnvironmentObject private var jit: JITManager
    @EnvironmentObject private var settings: AppSettings

    var body: some View {
        List {
            Section {
                HStack(spacing: 16) {
                    if jit.isPreparing {
                        ProgressView().controlSize(.large).frame(width: 54, height: 54)
                    } else {
                        Image(systemName: jit.isAvailable ? "bolt.circle.fill" : "bolt.slash.circle.fill")
                            .font(.system(size: 54))
                            .foregroundStyle(jit.isAvailable ? .green : .orange)
                    }
                    VStack(alignment: .leading, spacing: 6) {
                        Text(jit.isPreparing ? "JIT hazırlanıyor…" : jit.isAvailable ? "JIT etkin" : "JIT etkin değil").font(.title2.weight(.bold))
                        Text(jit.isAvailable
                             ? "x86-64 kodu çalışma anında ARM64 koduna çevrilip doğrudan M serisi çip üzerinde çalıştırılıyor."
                             : "Programlar şu an yorumlayıcı ile çalışır. JIT için Soul bir hata ayıklayıcı (StikDebug) tarafından başlatılmalı.")
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 8)
                if jit.recoveredFromCrash {
                    Label("Önceki açılışta JIT hazırlığı sırasında uygulama kapandı. StikDebug'da Soul için universal.js betiğinin atandığından emin olun.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                if jit.isDebugged && !jit.isAvailable && !jit.isPreparing {
                    Label("Hata ayıklayıcı bağlı (CS_DEBUGGED) ama JIT belleği hazırlanamadı. Ayrıntılar aşağıdaki günlükte.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
            }

            Section("Durum") {
                LabeledContent("Hata ayıklayıcı bayrağı (CS_DEBUGGED)", value: jit.isDebugged ? "Var" : "Yok")
                LabeledContent("csops bayrakları", value: "0x" + String(jit.csFlags, radix: 16))
                LabeledContent("Paket kimliği", value: jit.bundleID)
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
                Button { jit.openStikDebug() } label: { Label("StikDebug ile JIT'i etkinleştir (universal.js)", systemImage: "arrow.up.forward.app") }
                Button { jit.retry() } label: { Label("Durumu yenile / JIT'i yeniden dene", systemImage: "arrow.clockwise") }
                    .disabled(jit.isPreparing)
                Button { UIPasteboard.general.string = jit.diagnostics } label: { Label("Tanılama bilgisini kopyala", systemImage: "doc.on.doc") }
            } footer: {
                Text("StikDebug'dan Soul'a geri döndüğünüzde Soul hata ayıklayıcıyı otomatik algılar ve JIT belleğini hazırlar.")
            }

            Section("JIT günlüğü") {
                Text(jit.log.isEmpty ? "Henüz bir JIT denemesi yapılmadı." : jit.log)
                    .font(.footnote.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Section("JIT nasıl açılır? (iPad Air M3 dahil)") {
                Step(n: 1, title: "Soul'u yan yükleyin", text: "Soul.ipa dosyasını SideStore, AltStore ya da Sideloadly ile kendi Apple kimliğinizle imzalayıp yükleyin. Geliştirici imzası 'get-task-allow' yetkisini ekler; JIT için bu gereklidir.")
                Step(n: 2, title: "StikDebug'ı kurun", text: "StikDebug'ı yükleyin, eşleştirme (pairing) dosyasını içe aktarın ve LocalDevVPN'i açın.")
                Step(n: 3, title: "iPadOS 26 ve üzeri (TXM)", text: "StikDebug ayarlarında 'Always Run Scripts' ve 'Picture in Picture' seçeneklerini açın; Scripts bölümünde Soul için universal.js betiğini atayın. Soul, hata ayıklayıcı bağlanınca brk #0xf00d ile JIT bölgesi ister; StikDebug arka planda (PiP) açık kalmalıdır.")
                Step(n: 4, title: "iPadOS 17–18", text: "Ek betik gerekmez: StikDebug'da 'Enable JIT' ile Soul'u seçmeniz yeterlidir (Klasik yöntem).")
                Step(n: 5, title: "JIT'i etkinleştirin", text: "Bu ekrandaki 'StikDebug ile JIT'i etkinleştir' düğmesine basın ya da StikDebug → Enable JIT → Soul seçin. Soul'a dönünce birkaç saniye içinde 'JIT etkin' görünmelidir. Görünmezse 'Tanılama bilgisini kopyala' ile günlüğü paylaşın.")
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
