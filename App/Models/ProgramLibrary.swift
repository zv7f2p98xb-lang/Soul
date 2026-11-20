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
    }

    // MARK: persistence

    private func load() {
        guard let data = try? Data(contentsOf: SoulPaths.libraryFile),
              let list = try? JSONDecoder().decode([WinProgram].self, from: data) else { return }
        programs = list.filter { FileManager.default.fileExists(atPath: $0.hostURL.path) }
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
        guard ext == "exe" else {
            throw NSError(domain: "Soul", code: 2, userInfo: [NSLocalizedDescriptionKey: "Yalnızca .exe dosyaları veya program klasörleri kurulabilir. ZIP arşivlerini önce Dosyalar uygulamasında açın."])
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
        if p.summary.isEmpty {
            p.summary = p.compatibilityNote ?? (info.valid != 0 ? "64-bit Windows konsol programı" : "Windows programı")
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
