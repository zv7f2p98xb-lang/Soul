import SwiftUI

struct ProgramDetailView: View {
    @EnvironmentObject private var library: ProgramLibrary
    @EnvironmentObject private var sessions: SessionManager
    @Environment(\.dismiss) private var dismiss
    let programID: UUID
    @State private var arguments = ""
    @State private var showAddFiles = false
    @State private var confirmDelete = false

    var body: some View {
        if let program = library.program(withID: programID) {
            Form {
                Section {
                    HStack(spacing: 16) {
                        ProgramIcon(program: program, size: 72)
                        VStack(alignment: .leading, spacing: 6) {
                            Text(program.name).font(.title2.weight(.bold))
                            Text(program.summary).foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 6)
                    Button {
                        var p = program
                        p.arguments = arguments
                        library.update(p)
                        sessions.launch(p)
                        dismiss()
                    } label: {
                        Label(program.isInstaller ? "Kur" : "Çalıştır", systemImage: program.isInstaller ? "square.and.arrow.down.fill" : "play.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    if program.isInstaller {
                        Label("Kurulum sihirbazı açılır; kurulan programlar bittiğinde kitaplığa otomatik eklenir.", systemImage: "info.circle")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                if let note = program.compatibilityNote {
                    Section { Label(note, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
                }
                Section("Komut satırı argümanları") {
                    TextField("ör. --hızlı dosya.txt", text: $arguments)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.body.monospaced())
                        .onSubmit {
                            var p = program
                            p.arguments = arguments
                            library.update(p)
                        }
                }
                Section("Bilgiler") {
                    LabeledContent("Windows yolu", value: program.windowsPath)
                    LabeledContent("Mimari", value: program.is64Bit ? "x86-64 (AMD64)" : "x86 (32-bit)")
                    LabeledContent("Tür", value: program.isInstaller ? "Kurulum programı (\(program.installerKind ?? ""))" : program.isGUI ? "Grafik arayüz (GUI)" : "Konsol")
                    if program.installedBySetup == true { LabeledContent("Kaynak", value: "Kurulum ile yüklendi") }
                    LabeledContent("Eklenme", value: program.addedAt.formatted(date: .abbreviated, time: .shortened))
                    if let last = program.lastRun {
                        LabeledContent("Son çalıştırma", value: last.formatted(date: .abbreviated, time: .shortened))
                    }
                    LabeledContent("Çalıştırma sayısı", value: "\(program.runCount)")
                }
                if !program.importsSummary.isEmpty {
                    Section("Kullandığı DLL'ler") {
                        Text(program.importsSummary.replacingOccurrences(of: ",", with: "  ·  "))
                            .font(.footnote.monospaced())
                            .foregroundStyle(.secondary)
                    }
                }
                Section {
                    Button { showAddFiles = true } label: { Label("Klasöre dosya ekle (DLL, veri…)", systemImage: "doc.badge.plus") }
                    Button { openInFiles(program.folderURL) } label: { Label("Klasörü Dosyalar'da göster", systemImage: "folder") }
                    Button(role: .destructive) { confirmDelete = true } label: { Label("Programı sil", systemImage: "trash") }
                }
            }
            .navigationTitle(program.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Kapat") { dismiss() } }
            }
            .onAppear { arguments = program.arguments }
            .fileImporter(isPresented: $showAddFiles, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { library.addFiles(urls, to: program) }
            }
            .confirmationDialog("\"\(program.name)\" silinsin mi?", isPresented: $confirmDelete) {
                Button("Kitaplıktan kaldır") { library.remove(program, deleteFiles: false); dismiss() }
                if !program.isSample {
                    Button("Dosyalarıyla birlikte sil", role: .destructive) { library.remove(program, deleteFiles: true); dismiss() }
                }
            }
        } else {
            ContentUnavailableView("Program bulunamadı", systemImage: "questionmark.app")
        }
    }
}
