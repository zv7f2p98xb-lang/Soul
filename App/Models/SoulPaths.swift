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
    /// Setup programs the user adds are kept here (C:\users\soul\Downloads).
    static var downloads: URL { driveC.appendingPathComponent("users/soul/Downloads", isDirectory: true) }
    /// Folders where installers put their shortcuts (Start Menu and desktops).
    static var shortcutFolders: [URL] {
        ["ProgramData/Microsoft/Windows/Start Menu/Programs",
         "users/soul/AppData/Roaming/Microsoft/Windows/Start Menu/Programs",
         "users/soul/Desktop", "users/Public/Desktop"].map { driveC.appendingPathComponent($0, isDirectory: true) }
    }

    /// "C:\Program Files\App\app.exe" -> the file in drive_c (names compared case-insensitively,
    /// like Windows does). nil when it does not exist.
    static func hostURL(forWindowsPath win: String) -> URL? {
        var path = win.replacingOccurrences(of: "/", with: "\\")
        if path.count >= 2, path[path.index(after: path.startIndex)] == ":" { path = String(path.dropFirst(2)) }
        let parts = path.split(separator: "\\").map(String.init).filter { !$0.isEmpty && $0 != "." }
        var current = driveC
        let fm = FileManager.default
        for part in parts {
            let direct = current.appendingPathComponent(part)
            if fm.fileExists(atPath: direct.path) { current = direct; continue }
            guard let names = try? fm.contentsOfDirectory(atPath: current.path),
                  let match = names.first(where: { $0.caseInsensitiveCompare(part) == .orderedSame }) else { return nil }
            current = current.appendingPathComponent(match)
        }
        return current
    }

    static func ensure() {
        let fm = FileManager.default
        for dir in [root, driveC, programFiles, system32, icons,
                    driveC.appendingPathComponent("users/soul/Documents", isDirectory: true),
                    driveC.appendingPathComponent("users/soul/Temp", isDirectory: true), downloads] {
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
