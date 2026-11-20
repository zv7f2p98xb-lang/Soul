import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var library: ProgramLibrary
    @EnvironmentObject private var jit: JITManager

    var body: some View {
        Form {
            Section {
                Toggle("JIT kullan", isOn: $settings.useJIT)
                Picker("JIT yöntemi", selection: $settings.jitMethod) {
                    ForEach(AppSettings.JITMethod.allCases) { Text($0.title).tag($0) }
                }
                Stepper(value: $settings.jitMemoryMB, in: 16...512, step: 16) {
                    LabeledContent("Kod önbelleği", value: "\(settings.jitMemoryMB) MB")
                }
            } header: {
                Text("Çeviri motoru")
            } footer: {
                Text(settings.jitMethod.detail + " Yöntem ve bellek değişiklikleri bir sonraki JIT başlatmasında geçerli olur.")
            }

            Section("Konsol") {
                HStack {
                    Text("Yazı boyutu")
                    Slider(value: $settings.consoleFontSize, in: 10...24, step: 1)
                    Text("\(Int(settings.consoleFontSize))").monospacedDigit().frame(width: 28)
                }
                Toggle("Ayrıntılı günlük (yükleyici ve API uyarıları)", isOn: $settings.verboseLog)
                Toggle("Her zaman koyu tema", isOn: $settings.forceDarkMode)
            }

            Section("Veriler") {
                Button { library.rescan() } label: { Label("C: sürücüsünde yeni programları tara", systemImage: "arrow.clockwise") }
                Button { library.reinstallSamples() } label: { Label("Örnek programları yeniden yükle", systemImage: "shippingbox") }
                Button { openInFiles(SoulPaths.driveC) } label: { Label("C: sürücüsünü Dosyalar'da aç", systemImage: "externaldrive") }
                LabeledContent("C: sürücüsü konumu", value: "Dosyalar › iPad'imde › Soul › Soul › drive_c")
            }

            Section("Sürüm") {
                LabeledContent("Soul", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "-")
                LabeledContent("Çekirdek", value: String(cString: soul_version()))
            }
        }
        .navigationTitle("Ayarlar")
    }
}
