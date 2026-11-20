import Foundation

/// A Windows program installed into Soul's C: drive.
struct WinProgram: Identifiable, Codable, Hashable {
    var id: UUID = UUID()
    var name: String
    /// Path relative to drive_c using "/" separators, e.g. "Program Files/Foo/foo.exe".
    var relativePath: String
    var arguments: String = ""
    var summary: String = ""
    var addedAt: Date = Date()
    var lastRun: Date?
    var runCount: Int = 0
    var isSample: Bool = false
    var isGUI: Bool = false
    var is64Bit: Bool = true
    var importsSummary: String = ""

    var hostURL: URL { SoulPaths.driveC.appendingPathComponent(relativePath) }
    var folderURL: URL { hostURL.deletingLastPathComponent() }
    var windowsPath: String { SoulPaths.windowsPath(forRelative: relativePath) }
    var iconURL: URL { SoulPaths.icons.appendingPathComponent(id.uuidString + ".ico") }
    var fileName: String { (relativePath as NSString).lastPathComponent }

    var compatibilityNote: String? {
        if !is64Bit { return "32-bit (x86) programlar henüz desteklenmiyor." }
        if isGUI { return "Grafik arayüzlü (GUI) program: pencere çizimi henüz desteklenmiyor, yalnızca konsol çıktısı ve mesaj kutuları görünür." }
        return nil
    }
}
