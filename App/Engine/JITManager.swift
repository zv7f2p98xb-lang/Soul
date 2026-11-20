import Foundation
import UIKit

/// Detects whether the process may create executable memory and sets up the
/// JIT code region (see SoulCore/src/jit_mem.c for the low level details).
final class JITManager: ObservableObject {
    static let shared = JITManager()

    @Published private(set) var isDebugged = false
    @Published private(set) var isAvailable = false
    @Published private(set) var method = ""
    @Published private(set) var lastMessage = ""
    @Published private(set) var recoveredFromCrash = false

    private let inProgressKey = "jit.initInProgress"

    var osVersion: String { UIDevice.current.systemVersion }
    var osMajor: Int { Int(osVersion.split(separator: ".").first ?? "0") ?? 0 }
    var usesTXM: Bool {
        switch AppSettings.shared.jitMethod {
        case .txm: return true
        case .legacy, .off: return false
        case .automatic: return osMajor >= 26
        }
    }

    var deviceModel: String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: info.machine) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    func bootstrap() {
        let defaults = UserDefaults.standard
        if defaults.bool(forKey: inProgressKey) {
            // The previous launch died while asking the debugger for JIT memory
            // (e.g. TXM request without StikDebug's script). Fall back to safe settings.
            recoveredFromCrash = true
            defaults.set(false, forKey: inProgressKey)
            if AppSettings.shared.jitMethod != .off { AppSettings.shared.jitMethod = .legacy }
        }
        refresh()
        if isDebugged && AppSettings.shared.useJIT && AppSettings.shared.jitMethod != .off {
            initialize()
        }
    }

    func refresh() {
        isDebugged = soul_jit_is_debugged() != 0
        isAvailable = soul_jit_available() != 0
        method = String(cString: soul_jit_method())
    }

    @discardableResult
    func initialize() -> Bool {
        let defaults = UserDefaults.standard
        let mode: SoulJitMode
        switch AppSettings.shared.jitMethod {
        case .automatic: mode = SOUL_JIT_AUTO
        case .legacy: mode = SOUL_JIT_LEGACY
        case .txm: mode = SOUL_JIT_TXM
        case .off: mode = SOUL_JIT_OFF
        }
        defaults.set(true, forKey: inProgressKey)
        defaults.synchronize()
        var buffer = [CChar](repeating: 0, count: 512)
        let size = max(16, min(AppSettings.shared.jitMemoryMB, 512)) << 20
        let ok = soul_jit_init(mode, size, &buffer, buffer.count)
        defaults.set(false, forKey: inProgressKey)
        lastMessage = String(cString: buffer)
        refresh()
        return ok != 0
    }

    /// Opens StikDebug so the user can enable JIT for Soul.
    func openStikDebug() {
        open(schemes: ["stikdebug://", "stikjit://"])
    }

    private func open(schemes: [String]) {
        guard let first = schemes.first, let url = URL(string: first) else {
            lastMessage = "StikDebug bulunamadı. Önce StikDebug'ı yükleyin (bkz. kurulum adımları)."
            return
        }
        UIApplication.shared.open(url, options: [:]) { opened in
            if !opened {
                DispatchQueue.main.async { self.open(schemes: Array(schemes.dropFirst())) }
            }
        }
    }
}
