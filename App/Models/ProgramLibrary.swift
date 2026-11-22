import Foundation
import UIKit
import ImageIO

/// Keeps the list of installed programs and handles importing ("installing")
/// .exe files or whole program folders into C:\Program Files.
@MainActor
final class ProgramLibrary: ObservableObject {
    static let shared = ProgramLibrary()

    @Published private(set) var programs: [WinProgram] = []
    @Published var banner: String?
    @Published var isImporting = false

    private var iconCache: [UUID: UIImage] = [:]

    private init() {
        SoulPaths.ensure()
        load()
        installSamplesIfNeeded()
        _ = scanShortcuts()
    }

    // MARK: persistence

    private func load() {
        guard let data = try? Data(contentsOf: SoulPaths.libraryFile),
              let list = try? JSONDecoder().decode([WinProgram].self, from: data) else { return }
        programs = list.filter { FileManager.default.fileExists(atPath: $0.hostURL.path) }.map { p in
            // programs added before 32-bit support kept the old "not supported" note as their summary
            var p = p
            if !p.is64Bit && p.summary.hasPrefix("32-bit (x86) programlar henüz") {
                p.summary = p.isGUI ? "Pencereli bilgisayar programı (x86, 32-bit)" : "Konsol programı (x86, 32-bit)"
            }
            return p
        }
    }

    private func save() {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? enc.encode(programs) {
            try? data.write(to: SoulPaths.libraryFile, options: .atomic)
        }
    }

    func update(_ program: WinProgram) {
        guard let i = programs.firstIndex(where: { $0.id == program.id }) else { return }
        programs[i] = program
        save()
    }

    func markRun(_ program: WinProgram) {
        guard let i = programs.firstIndex(where: { $0.id == program.id }) else { return }
        programs[i].lastRun = Date()
        programs[i].runCount += 1
        save()
    }

    func program(withID id: UUID) -> WinProgram? { programs.first { $0.id == id } }

    // MARK: importing

    /// Imports files or folders picked in the document picker, dropped, or opened from Files.
    func importItems(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        isImporting = true
        Task.detached(priority: .userInitiated) {
            var added: [WinProgram] = []
            var errors: [String] = []
            for url in urls {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                do {
                    added += try Self.install(url)
                } catch {
                    errors.append("\(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            await MainActor.run {
                self.isImporting = false
                for p in added { self.register(p) }
                self.save()
                if !errors.isEmpty {
                    self.banner = "İçe aktarma hatası: " + errors.joined(separator: "\n")
                } else if added.isEmpty {
                    self.banner = "Seçilen öğelerde .exe dosyası bulunamadı."
                } else {
                    self.banner = added.count == 1 ? "\"\(added[0].name)\" kuruldu." : "\(added.count) program kuruldu."
                }
            }
        }
    }

    /// Copies additional files (DLLs, data) into an installed program's folder.
    func addFiles(_ urls: [URL], to program: WinProgram) {
        let fm = FileManager.default
        var count = 0
        for url in urls {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let dest = program.folderURL.appendingPathComponent(url.lastPathComponent)
            try? fm.removeItem(at: dest)
            if (try? fm.copyItem(at: url, to: dest)) != nil { count += 1 }
        }
        banner = "\(count) dosya \"\(program.name)\" klasörüne eklendi."
    }

    private func register(_ program: WinProgram) {
        if programs.contains(where: { $0.relativePath == program.relativePath }) { return }
        programs.append(program)
        programs.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    nonisolated private static func install(_ source: URL) throws -> [WinProgram] {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: source.path, isDirectory: &isDir) else {
            throw NSError(domain: "Soul", code: 1, userInfo: [NSLocalizedDescriptionKey: "Dosya bulunamadı"])
        }
        // Already inside drive_c (e.g. opened in place from Files): just register it.
        if let rel = SoulPaths.relativeToDriveC(source) {
            if isDir.boolValue {
                return findExecutables(in: source).compactMap { makeProgram(at: $0) }
            }
            return [makeProgram(relativePath: rel)].compactMap { $0 }
        }
        if isDir.boolValue {
            let dest = SoulPaths.uniqueDirectory(in: SoulPaths.programFiles, name: source.lastPathComponent)
            try fm.copyItem(at: source, to: dest)
            return findExecutables(in: dest).compactMap { makeProgram(at: $0) }
        }
        let ext = source.pathExtension.lowercased()
        if ext == "msi" {
            throw NSError(domain: "Soul", code: 3, userInfo: [NSLocalizedDescriptionKey: "MSI (Windows Installer) paketleri henüz desteklenmiyor. Programın .exe kurulum dosyasını deneyin."])
        }
        guard ext == "exe" else {
            throw NSError(domain: "Soul", code: 2, userInfo: [NSLocalizedDescriptionKey: "Yalnızca .exe dosyaları veya program klasörleri kurulabilir. ZIP arşivlerini önce Dosyalar uygulamasında açın."])
        }
        // A setup program is kept in C:\users\soul\Downloads; what it installs goes to Program Files.
        if installerKind(at: source) != nil {
            let dest = SoulPaths.downloads.appendingPathComponent(source.lastPathComponent)
            try? fm.removeItem(at: dest)
            try fm.copyItem(at: source, to: dest)
            return [makeProgram(at: dest)].compactMap { $0 }
        }
        let base = source.deletingPathExtension().lastPathComponent
        let dir = SoulPaths.uniqueDirectory(in: SoulPaths.programFiles, name: base)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let dest = dir.appendingPathComponent(source.lastPathComponent)
        try fm.copyItem(at: source, to: dest)
        return [makeProgram(at: dest)].compactMap { $0 }
    }

    nonisolated private static func findExecutables(in dir: URL) -> [URL] {
        guard let en = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.isRegularFileKey],
                                                      options: [.skipsHiddenFiles]) else { return [] }
        var result: [URL] = []
        for case let url as URL in en {
            if en.level > 4 { en.skipDescendants(); continue }
            let name = url.lastPathComponent.lowercased()
            guard url.pathExtension.lowercased() == "exe" else { continue }
            if name.hasPrefix("unins") || name.contains("setup") && result.count > 0 { continue }
            result.append(url)
        }
        return result.sorted { $0.path.count < $1.path.count }
    }

    nonisolated private static func makeProgram(at url: URL) -> WinProgram? {
        guard let rel = SoulPaths.relativeToDriveC(url) else { return nil }
        return makeProgram(relativePath: rel)
    }

    nonisolated private static func makeProgram(relativePath rel: String, name: String? = nil, sample: Bool = false,
                                                summary: String = "", args: String = "") -> WinProgram? {
        let url = SoulPaths.driveC.appendingPathComponent(rel)
        var p = WinProgram(name: name ?? url.deletingPathExtension().lastPathComponent, relativePath: rel)
        p.isSample = sample
        p.summary = summary
        p.arguments = args
        var info = SoulPEInfo()
        _ = url.path.withCString { soul_pe_inspect($0, &info) }
        p.is64Bit = info.valid != 0 || info.machine == 0x8664
        if info.machine == 0x14C { p.is64Bit = false }
        p.isGUI = info.is_gui != 0
        p.importsSummary = withUnsafeBytes(of: info.imports) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
        if !sample { p.installerKind = installerKind(at: url) }
        if p.isInstaller && summary.isEmpty { p.summary = "Kurulum programı (\(p.installerKind ?? "")) — kurulan programlar kitaplığa eklenir" }
        if p.summary.isEmpty {
            let arch = p.is64Bit ? "x64" : "x86, 32-bit"
            p.summary = p.compatibilityNote ?? (info.valid == 0 ? "Bilgisayar programı" : info.is_gui != 0 ? "Pencereli bilgisayar programı (\(arch))" : "Konsol programı (\(arch))")
        }
        extractIcon(for: p)
        return p
    }

    nonisolated private static func extractIcon(for p: WinProgram) {
        var buf: UnsafeMutablePointer<UInt8>?
        var len = 0
        let ok = p.hostURL.path.withCString { soul_pe_extract_icon($0, &buf, &len) }
        if ok == 0, let buf, len > 0 {
            let data = Data(bytes: buf, count: len)
            soul_free(buf)
            try? data.write(to: p.iconURL)
        }
    }

    // MARK: installers

    /// Recognizes setup programs by their name or by the installer engine inside the file.
    nonisolated static func installerKind(at url: URL) -> String? {
        let name = url.deletingPathExtension().lastPathComponent.lowercased()
        if name.hasPrefix("unins") || name.hasPrefix("uninst") { return nil }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        let head = (try? handle.read(upToCount: 8 << 20)) ?? Data()
        let signatures: [(String, String)] = [("Inno Setup Setup Data", "Inno Setup"), ("Nullsoft Install System", "NSIS"),
                                              ("NullsoftInst", "NSIS"), ("InstallShield", "InstallShield"), (".wixburn", "WiX"),
                                              (";!@Install@!UTF-8!", "7-Zip SFX"), ("Setup Factory", "Setup Factory"),
                                              ("WiseMain", "Wise"), ("InstallAware", "InstallAware")]
        for (sig, kind) in signatures where head.range(of: Data(sig.utf8)) != nil { return kind }
        if name.contains("setup") || name.contains("install") { return "Kurulum" }
        return nil
    }

    /// Adds the programs installers created shortcuts for (Start Menu, desktop). Returns the new ones.
    @discardableResult
    func scanShortcuts() -> [WinProgram] {
        let fm = FileManager.default
        var added: [WinProgram] = []
        for folder in SoulPaths.shortcutFolders {
            guard let en = fm.enumerator(at: folder, includingPropertiesForKeys: nil) else { continue }
            for case let url as URL in en where url.pathExtension.lowercased() == "lnk" {
                var info = SoulLnkInfo()
                guard url.path.withCString({ soul_lnk_read($0, &info) }) == 0 else { continue }
                let target = withUnsafeBytes(of: info.target) { raw in String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self) }
                guard target.lowercased().hasSuffix(".exe"), let host = SoulPaths.hostURL(forWindowsPath: target),
                      let rel = SoulPaths.relativeToDriveC(host) else { continue }
                let exeName = host.deletingPathExtension().lastPathComponent.lowercased()
                if exeName.hasPrefix("unins") || exeName.hasPrefix("uninst") { continue }
                if programs.contains(where: { $0.relativePath.caseInsensitiveCompare(rel) == .orderedSame }) || added.contains(where: { $0.relativePath == rel }) { continue }
                let title = url.deletingPathExtension().lastPathComponent
                let args = withUnsafeBytes(of: info.arguments) { raw in String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self) }
                guard var p = Self.makeProgram(relativePath: rel, name: title, args: args) else { continue }
                p.installedBySetup = true
                p.installerKind = nil
                added.append(p)
            }
        }
        for p in added { register(p) }
        if !added.isEmpty { save() }
        return added
    }

    /// Called when a program ends: an installer may have added programs.
    func programEnded(_ program: WinProgram) {
        let added = scanShortcuts()
        if !added.isEmpty {
            let names = added.map(\.name).joined(separator: ", ")
            banner = added.count == 1 ? "Kuruldu: \(names) kitaplığa eklendi." : "Kuruldu: \(names) kitaplığa eklendi."
        } else if program.isInstaller {
            // installers without shortcuts: look for new programs in Program Files
            let before = Set(programs.map(\.relativePath))
            rescan()
            let new = programs.filter { !before.contains($0.relativePath) }
            banner = new.isEmpty ? "Kurulum bitti." : "Kuruldu: \(new.map(\.name).joined(separator: ", ")) kitaplığa eklendi."
        }
    }

    // MARK: samples

    private func installSamplesIfNeeded() {
        guard let bundled = Bundle.main.url(forResource: "Samples", withExtension: nil),
              let manifestData = try? Data(contentsOf: bundled.appendingPathComponent("samples.json")),
              let manifest = try? JSONDecoder().decode([SampleEntry].self, from: manifestData) else { return }
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
        let key = "samplesInstalledVersion"
        let fm = FileManager.default
        let dest = SoulPaths.programFiles.appendingPathComponent("Soul Örnekleri", isDirectory: true)
        let alreadyInstalled = UserDefaults.standard.string(forKey: key) == version && fm.fileExists(atPath: dest.path)
        if !alreadyInstalled {
            try? fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for entry in manifest {
                let src = bundled.appendingPathComponent(entry.file)
                let dst = dest.appendingPathComponent(entry.file)
                try? fm.removeItem(at: dst)
                try? fm.copyItem(at: src, to: dst)
            }
            UserDefaults.standard.set(version, forKey: key)
        }
        for entry in manifest where !programs.contains(where: { $0.relativePath == "Program Files/Soul Örnekleri/\(entry.file)" }) {
            if let p = Self.makeProgram(relativePath: "Program Files/Soul Örnekleri/\(entry.file)", name: entry.name,
                                        sample: true, summary: entry.description, args: entry.args) {
                register(p)
            }
        }
        save()
    }

    func reinstallSamples() {
        UserDefaults.standard.removeObject(forKey: "samplesInstalledVersion")
        programs.removeAll { $0.isSample }
        installSamplesIfNeeded()
        banner = "Örnek programlar yeniden yüklendi."
    }

    private struct SampleEntry: Decodable {
        let file: String
        let name: String
        let description: String
        let args: String
    }

    // MARK: maintenance

    /// Finds .exe files that were copied into drive_c with the Files app.
    func rescan() {
        let found = Self.findExecutables(in: SoulPaths.programFiles)
        var added = 0
        for url in found {
            guard let rel = SoulPaths.relativeToDriveC(url),
                  !programs.contains(where: { $0.relativePath == rel }),
                  let p = Self.makeProgram(relativePath: rel) else { continue }
            register(p)
            added += 1
        }
        programs.removeAll { !FileManager.default.fileExists(atPath: $0.hostURL.path) }
        save()
        banner = added > 0 ? "\(added) yeni program bulundu." : "Yeni program bulunamadı."
    }

    func remove(_ program: WinProgram, deleteFiles: Bool) {
        programs.removeAll { $0.id == program.id }
        iconCache[program.id] = nil
        try? FileManager.default.removeItem(at: program.iconURL)
        if deleteFiles && !program.isSample {
            let folder = program.folderURL
            // only delete whole folders that live directly under Program Files
            if folder.deletingLastPathComponent().standardizedFileURL == SoulPaths.programFiles.standardizedFileURL {
                try? FileManager.default.removeItem(at: folder)
            } else {
                try? FileManager.default.removeItem(at: program.hostURL)
            }
        }
        save()
    }

    func icon(for program: WinProgram) -> UIImage? {
        if let cached = iconCache[program.id] { return cached }
        guard let data = try? Data(contentsOf: program.iconURL),
              let src = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        let count = CGImageSourceGetCount(src)
        var best: CGImage?
        for i in 0..<max(count, 1) {
            if let img = CGImageSourceCreateImageAtIndex(src, i, nil), img.width >= (best?.width ?? 0) { best = img }
        }
        guard let best else { return nil }
        let image = UIImage(cgImage: best)
        iconCache[program.id] = image
        return image
    }
}
