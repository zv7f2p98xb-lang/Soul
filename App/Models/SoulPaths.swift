import Foundation

/// Directory layout inside the app's Documents folder (visible in the Files app):
///
///     Soul/
///       drive_c/                 <- mapped to C:\
///         Program Files/<App>/   <- installed programs
///         users/soul/...         <- profile, AppData, Temp
///         windows/system32/      <- extra DLLs can be placed here
///       Icons/                   <- extracted program icons
///       library.json
enum SoulPaths {
    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }
    static var root: URL { documents.appendingPathComponent("Soul", isDirectory: true) }
    static var driveC: URL { root.appendingPathComponent("drive_c", isDirectory: true) }
    static var programFiles: URL { driveC.appendingPathComponent("Program Files", isDirectory: true) }
    static var system32: URL { driveC.appendingPathComponent("windows/system32", isDirectory: true) }
    static var icons: URL { root.appendingPathComponent("Icons", isDirectory: true) }
    static var libraryFile: URL { root.appendingPathComponent("library.json") }

    static func ensure() {
        let fm = FileManager.default
        for dir in [root, driveC, programFiles, system32, icons,
                    driveC.appendingPathComponent("users/soul/Documents", isDirectory: true),
                    driveC.appendingPathComponent("users/soul/Temp", isDirectory: true)] {
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
    }

    /// "Program Files/App/app.exe" -> "C:\Program Files\App\app.exe"
    static func windowsPath(forRelative relative: String) -> String {
        "C:\\" + relative.replacingOccurrences(of: "/", with: "\\")
    }

    /// Path of `url` relative to drive_c (nil if outside).
    static func relativeToDriveC(_ url: URL) -> String? {
        let base = driveC.standardizedFileURL.resolvingSymlinksInPath().path
        let full = url.standardizedFileURL.resolvingSymlinksInPath().path
        guard full.hasPrefix(base + "/") else { return nil }
        return String(full.dropFirst(base.count + 1))
    }

    /// Returns a directory URL inside `parent` named `name`, adding " (2)", " (3)"... if needed.
    static func uniqueDirectory(in parent: URL, name: String) -> URL {
        let fm = FileManager.default
        var candidate = parent.appendingPathComponent(name, isDirectory: true)
        var n = 2
        while fm.fileExists(atPath: candidate.path) {
            candidate = parent.appendingPathComponent("\(name) (\(n))", isDirectory: true)
            n += 1
        }
        return candidate
    }
}
