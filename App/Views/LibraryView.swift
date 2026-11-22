import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct LibraryView: View {
    @EnvironmentObject private var library: ProgramLibrary
    @EnvironmentObject private var sessions: SessionManager
    @EnvironmentObject private var jit: JITManager
    @State private var showImporter = false
    @State private var search = ""
    @State private var detail: WinProgram?
    @State private var pendingDelete: WinProgram?

    private var filtered: [WinProgram] {
        let list = library.programs
        guard !search.isEmpty else { return list }
        return list.filter { $0.name.localizedCaseInsensitiveContains(search) || $0.relativePath.localizedCaseInsensitiveContains(search) }
    }
    private var userPrograms: [WinProgram] { filtered.filter { !$0.isSample && !$0.isInstaller } }
    private var installers: [WinProgram] { filtered.filter { $0.isInstaller } }
    private var samples: [WinProgram] { filtered.filter { $0.isSample } }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                if !jit.isAvailable && !jit.isPreparing { JITBanner() }
                if library.programs.filter({ !$0.isSample }).isEmpty && search.isEmpty { EmptyLibraryCard { showImporter = true } }
                if !userPrograms.isEmpty { section(title: "Programlarım", items: userPrograms) }
                if !installers.isEmpty { section(title: "Kurulum Dosyaları", items: installers) }
                if !samples.isEmpty { section(title: "Örnek Programlar", items: samples) }
            }
            .padding()
        }
        .navigationTitle("Kitaplık")
        .searchable(text: $search, prompt: "Program ara")
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Menu {
                    Button { library.rescan() } label: { Label("C: sürücüsünü tara", systemImage: "arrow.clockwise") }
                    Button { openInFiles(SoulPaths.programFiles) } label: { Label("Dosyalar'da göster", systemImage: "folder") }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                Button { showImporter = true } label: {
                    Label("Program Ekle", systemImage: "plus")
                }
                .keyboardShortcut("o", modifiers: .command)
            }
        }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.exe, .folder, UTType(filenameExtension: "msi") ?? .item, .item], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { library.importItems(urls) }
        }
        .dropDestination(for: URL.self) { urls, _ in
            library.importItems(urls)
            return true
        }
        .sheet(item: $detail) { program in
            NavigationStack { ProgramDetailView(programID: program.id) }
        }
        .confirmationDialog("Program silinsin mi?", isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }), presenting: pendingDelete) { p in
            Button("Kitaplıktan kaldır") { library.remove(p, deleteFiles: false) }
            if !p.isSample { Button("Dosyalarıyla birlikte sil", role: .destructive) { library.remove(p, deleteFiles: true) } }
        }
        .overlay(alignment: .bottom) { BannerView() }
        .overlay {
            if library.isImporting {
                ProgressView("Kuruluyor…").padding(24).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
            }
        }
    }

    @ViewBuilder
    private func section(title: String, items: [WinProgram]) -> some View {
        Text(title).font(.title3.weight(.semibold))
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 180, maximum: 260), spacing: 16)], spacing: 16) {
            ForEach(items) { program in
                ProgramCard(program: program)
                    .onTapGesture { detail = program }
                    .contextMenu {
                        Button { sessions.launch(program) } label: { Label(program.isInstaller ? "Kur" : "Çalıştır", systemImage: program.isInstaller ? "square.and.arrow.down" : "play.fill") }
                        Button { detail = program } label: { Label("Ayrıntılar", systemImage: "info.circle") }
                        Button { openInFiles(program.folderURL) } label: { Label("Klasörü göster", systemImage: "folder") }
                        Divider()
                        Button(role: .destructive) { pendingDelete = program } label: { Label("Sil", systemImage: "trash") }
                    }
            }
        }
    }
}

func openInFiles(_ url: URL) {
    var comps = URLComponents()
    comps.scheme = "shareddocuments"
    comps.path = url.path
    if let link = comps.url { UIApplication.shared.open(link) }
}

struct ProgramCard: View {
    @EnvironmentObject private var library: ProgramLibrary
    @EnvironmentObject private var sessions: SessionManager
    let program: WinProgram

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top) {
                ProgramIcon(program: program, size: 48)
                Spacer()
                Button { sessions.launch(program) } label: {
                    Image(systemName: program.isInstaller ? "square.and.arrow.down.fill" : "play.fill")
                        .font(.system(size: 14, weight: .bold))
                        .frame(width: 34, height: 34)
                        .background(Color.accentColor, in: Circle())
                        .foregroundStyle(.white)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(program.isInstaller ? "Kur" : "Çalıştır")
            }
            Text(program.name).font(.headline).lineLimit(1)
            Text(program.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2, reservesSpace: true)
            HStack(spacing: 6) {
                Badge(text: program.is64Bit ? "x64" : "x86", color: program.is64Bit ? .blue : .teal)
                Badge(text: program.isGUI ? "GUI" : "Konsol", color: program.isGUI ? .orange : .teal)
                if program.isSample { Badge(text: "Örnek", color: .purple) }
                if program.isInstaller { Badge(text: "Kurulum", color: .green) }
                if program.installedBySetup == true { Badge(text: "Kuruldu", color: .green) }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(Color.primary.opacity(0.06)))
        .contentShape(RoundedRectangle(cornerRadius: 18))
    }
}

struct Badge: View {
    let text: String
    let color: Color
    var body: some View {
        Text(text)
            .font(.caption2.weight(.bold))
            .padding(.horizontal, 7).padding(.vertical, 3)
            .background(color.opacity(0.15), in: Capsule())
            .foregroundStyle(color)
    }
}

struct ProgramIcon: View {
    @EnvironmentObject private var library: ProgramLibrary
    let program: WinProgram
    let size: CGFloat

    var body: some View {
        Group {
            if let image = library.icon(for: program) {
                Image(uiImage: image).resizable().interpolation(.high).scaledToFit().padding(size * 0.08)
            } else {
                Image(systemName: program.isGUI ? "macwindow" : "terminal")
                    .font(.system(size: size * 0.45, weight: .semibold))
                    .foregroundStyle(.white)
            }
        }
        .frame(width: size, height: size)
        .background(
            LinearGradient(colors: [Color(red: 0.26, green: 0.22, blue: 0.62), Color(red: 0.10, green: 0.55, blue: 0.75)],
                           startPoint: .topLeading, endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: size * 0.24, style: .continuous))
    }
}

struct JITBanner: View {
    @EnvironmentObject private var jit: JITManager
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: "bolt.slash.fill").font(.title2).foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 4) {
                Text("JIT etkin değil").font(.headline)
                Text("Programlar yorumlayıcı ile (yavaş) çalışır. Tam hız için Soul'u StikDebug üzerinden JIT ile başlatın.")
                    .font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            Button("JIT'i Etkinleştir") { jit.openStikDebug() }.buttonStyle(.borderedProminent).tint(.orange)
        }
        .padding()
        .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 16))
    }
}

struct EmptyLibraryCard: View {
    let action: () -> Void
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "shippingbox.and.arrow.backward.fill").font(.system(size: 44)).foregroundStyle(.tint)
            Text("Bilgisayar programlarını buraya ekleyin").font(.title3.weight(.semibold))
            Text("Bir .exe dosyasını, kurulum dosyasını (setup.exe) veya program klasörünü seçin ya da Dosyalar uygulamasından sürükleyip bırakın. Kurulum sihirbazları çalışır; kurulan programlar kitaplığa kendiliğinden eklenir.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary)
            Button(action: action) { Label("Program Ekle", systemImage: "plus") }.buttonStyle(.borderedProminent)
        }
        .padding(28)
        .frame(maxWidth: .infinity)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
    }
}

struct BannerView: View {
    @EnvironmentObject private var library: ProgramLibrary
    var body: some View {
        if let text = library.banner {
            Text(text)
                .font(.callout)
                .padding(.horizontal, 16).padding(.vertical, 10)
                .background(.thinMaterial, in: Capsule())
                .padding(.bottom, 20)
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .task(id: text) {
                    try? await Task.sleep(nanoseconds: 3_500_000_000)
                    withAnimation { library.banner = nil }
                }
        }
    }
}
