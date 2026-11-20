import Foundation
import UIKit

/// Detects whether the process may create executable memory and sets up the
/// JIT code region (see SoulCore/src/jit_mem.c for the low level details).
///
/// StikDebug attaches to Soul *after* it has started, so CS_DEBUGGED is polled
/// continuously; as soon as it appears the JIT region is prepared on a
/// background thread (the debugger round trip can take a few seconds).
final class JITManager: ObservableObject {
    static let shared = JITManager()

    @Published private(set) var isDebugged = false
    @Published private(set) var isAvailable = false
    @Published private(set) var isPreparing = false
    @Published private(set) var method = ""
    @Published private(set) var lastMessage = ""
    @Published private(set) var log = ""
    @Published private(set) var csFlags: UInt32 = 0
    @Published private(set) var recoveredFromCrash = false

    private let inProgressKey = "jit.initInProgress"
    private var pollTimer: Timer?
    private var attemptedWhileDebugged = false
    private let queue = DispatchQueue(label: "soul.jit.setup", qos: .userInitiated)

    var osVersion: String { UIDevice.current.systemVersion }
    var osMajor: Int { Int(osVersion.split(separator: ".").first ?? "0") ?? 0 }
    var bundleID: String { Bundle.main.bundleIdentifier ?? "com.soulemu.Soul" }

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
            // The previous launch died while asking the debugger for JIT memory.
            recoveredFromCrash = true
            defaults.set(false, forKey: inProgressKey)
        }
        refresh()
        startPolling()
    }

    /// Re-checks right away (called when the app becomes active again, e.g. after
    /// switching back from StikDebug).
    func checkNow() {
        refresh()
        attemptIfPossible()
        startPolling()
    }

    func refresh() {
        csFlags = soul_jit_cs_flags()
        isDebugged = soul_jit_is_debugged() != 0
        isAvailable = soul_jit_available() != 0
        method = String(cString: soul_jit_method())
        log = String(cString: soul_jit_log())
    }

    private func startPolling() {
        guard pollTimer == nil, !isAvailable else { return }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] timer in
            guard let self else { timer.invalidate(); return }
            self.refresh()
            if self.isAvailable {
                timer.invalidate()
                self.pollTimer = nil
                return
            }
            self.attemptIfPossible()
        }
    }

    private func attemptIfPossible() {
        guard isDebugged, !isAvailable, !isPreparing, !attemptedWhileDebugged else { return }
        guard AppSettings.shared.useJIT, AppSettings.shared.jitMethod != .off else { return }
        attemptedWhileDebugged = true
        initialize()
    }

    /// Prepares the JIT region on a background thread.
    func initialize() {
        guard !isPreparing else { return }
        let mode: SoulJitMode
        switch AppSettings.shared.jitMethod {
        case .automatic: mode = SOUL_JIT_AUTO
        case .legacy: mode = SOUL_JIT_LEGACY
        case .txm: mode = SOUL_JIT_TXM
        case .off: mode = SOUL_JIT_OFF
        }
        let size = max(16, min(AppSettings.shared.jitMemoryMB, 512)) << 20
        isPreparing = true
        let defaults = UserDefaults.standard
        defaults.set(true, forKey: inProgressKey)
        defaults.synchronize()
        queue.async {
            var buffer = [CChar](repeating: 0, count: 512)
            _ = soul_jit_init(mode, size, &buffer, buffer.count)
            let message = String(cString: buffer)
            DispatchQueue.main.async {
                defaults.set(false, forKey: self.inProgressKey)
                self.isPreparing = false
                self.lastMessage = message
                self.refresh()
            }
        }
    }

    /// Manual retry from the JIT screen.
    func retry() {
        attemptedWhileDebugged = false
        refresh()
        if isDebugged { attemptedWhileDebugged = true; initialize() }
        startPolling()
    }

    /// Opens StikDebug and asks it to enable JIT for this app with the universal
    /// script (needed on iPadOS 26+, harmless before).
    func openStikDebug() {
        var comps = URLComponents()
        comps.scheme = "stikdebug"
        comps.host = "enable-jit"
        comps.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "script-name", value: "universal.js"),
        ]
        var urls: [URL] = []
        if let u = comps.url { urls.append(u) }
        if let u = URL(string: "stikdebug://") { urls.append(u) }
        if let u = URL(string: "stikjit://") { urls.append(u) }
        open(urls)
        attemptedWhileDebugged = false
        startPolling()
    }

    private func open(_ urls: [URL]) {
        guard let first = urls.first else {
            lastMessage = "StikDebug bulunamadı. Önce StikDebug'ı yükleyin (bkz. kurulum adımları)."
            return
        }
        UIApplication.shared.open(first, options: [:]) { opened in
            if !opened { DispatchQueue.main.async { self.open(Array(urls.dropFirst())) } }
        }
    }

    /// Text the user can copy and send when reporting a problem.
    var diagnostics: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "-"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "-"
        return """
        Soul \(version) (\(build)) — \(String(cString: soul_version()))
        Cihaz: \(deviceModel), iPadOS \(osVersion)
        Paket kimliği: \(bundleID)
        csops bayrakları: 0x\(String(csFlags, radix: 16)) (CS_DEBUGGED=\(isDebugged ? 1 : 0))
        JIT: \(isAvailable ? "etkin" : "etkin değil") — \(method)
        Seçili yöntem: \(AppSettings.shared.jitMethod.title), bellek \(AppSettings.shared.jitMemoryMB) MB
        Son mesaj: \(lastMessage)
        Önceki açılışta JIT hazırlığında çökme: \(recoveredFromCrash ? "evet" : "hayır")
        --- JIT günlüğü ---
        \(log.isEmpty ? "(boş)" : log)
        """
    }
}
