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
    /// Set for setup programs ("Inno Setup", "NSIS", ...): running one installs other programs.
    var installerKind: String? = nil
    /// The program was found through a shortcut an installer created.
    var installedBySetup: Bool? = nil

    var isInstaller: Bool { installerKind != nil }

    /// Programs are identified by file and arguments: every Windows Installer entry runs the same msiexec.exe.
    static func identityKey(relativePath: String, arguments: String) -> String {
        relativePath.lowercased() + "\u{1}" + arguments
    }
    var identityKey: String { Self.identityKey(relativePath: relativePath, arguments: arguments) }

    /// Entry created for a .msi package (runs msiexec.exe /i "<package>").
    var isMSIPackage: Bool { installerKind == "Windows Installer (MSI)" && fileName.lowercased() == "msiexec.exe" }
    /// The package file of such an entry (the quoted Windows path in its arguments).
    var msiPackageURL: URL? {
        let parts = arguments.components(separatedBy: "\"")
        guard isMSIPackage, parts.count >= 3 else { return nil }
        return SoulPaths.hostURL(forWindowsPath: parts[1])
    }

    var hostURL: URL { SoulPaths.driveC.appendingPathComponent(relativePath) }
    var folderURL: URL { hostURL.deletingLastPathComponent() }
    var windowsPath: String { SoulPaths.windowsPath(forRelative: relativePath) }
    var iconURL: URL { SoulPaths.icons.appendingPathComponent(id.uuidString + ".ico") }
    var fileName: String { (relativePath as NSString).lastPathComponent }

    var compatibilityNote: String? {
        let imports = importsSummary.lowercased()
        if ["d3d10", "d3d11", "d3d12", "dxgi", "vulkan"].contains(where: { imports.contains($0) }) {
            return "Bu program DirectX 10/11/12 veya Vulkan kullanıyor; bunlar henüz desteklenmiyor (Direct3D 9, OpenGL, pencere ve GDI çizimleri çalışır)."
        }
        if imports.contains("d3d9") {
            // With Metal acceleration on (the default) there is nothing to warn about; the CPU renderer is the fallback.
            if AppSettings.shared.gpuAcceleration { return nil }
            return "GPU hızlandırma (Metal) kapalı: Direct3D 9 yazılımla çiziliyor; ağır oyunlar yavaş olabilir. Ayarlar'dan açabilirsiniz."
        }
        if imports.contains("mscoree") { return ".NET programları henüz desteklenmiyor." }
        return nil
    }
}
