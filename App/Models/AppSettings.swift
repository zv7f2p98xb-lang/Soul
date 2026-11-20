import Foundation
import SwiftUI

/// User preferences, persisted in UserDefaults.
final class AppSettings: ObservableObject {
    static let shared = AppSettings()

    enum JITMethod: Int, CaseIterable, Identifiable {
        case automatic = 0, legacy = 1, txm = 2, off = 3
        var id: Int { rawValue }
        var title: String {
            switch self {
            case .automatic: return "Otomatik"
            case .legacy: return "Klasik (iPadOS 17–18)"
            case .txm: return "TXM (iPadOS 26+)"
            case .off: return "Kapalı (yalnızca yorumlayıcı)"
            }
        }
        var detail: String {
            switch self {
            case .automatic: return "iPadOS 26 ve üzerinde TXM, daha eskilerde klasik yöntem kullanılır."
            case .legacy: return "Hata ayıklayıcı CS_DEBUGGED bayrağını koyduktan sonra RW/RX ayna bellek kullanır."
            case .txm: return "StikDebug'ın universal.js betiğinden brk #0xf00d ile JIT bölgesi ister."
            case .off: return "Tüm x86 komutları yorumlanır. Yavaştır ama JIT gerektirmez."
            }
        }
    }

    private let defaults = UserDefaults.standard

    @Published var jitMethod: JITMethod {
        didSet { defaults.set(jitMethod.rawValue, forKey: "jitMethod") }
    }
    @Published var useJIT: Bool {
        didSet { defaults.set(useJIT, forKey: "useJIT") }
    }
    @Published var jitMemoryMB: Int {
        didSet { defaults.set(jitMemoryMB, forKey: "jitMemoryMB") }
    }
    @Published var verboseLog: Bool {
        didSet { defaults.set(verboseLog, forKey: "verboseLog") }
    }
    @Published var consoleFontSize: Double {
        didSet { defaults.set(consoleFontSize, forKey: "consoleFontSize") }
    }
    @Published var forceDarkMode: Bool {
        didSet { defaults.set(forceDarkMode, forKey: "forceDarkMode") }
    }

    private init() {
        defaults.register(defaults: [
            "jitMethod": JITMethod.automatic.rawValue,
            "useJIT": true,
            "jitMemoryMB": 64,
            "verboseLog": false,
            "consoleFontSize": 14.0,
            "forceDarkMode": false,
        ])
        // Older builds switched permanently to the legacy method after a crash during
        // JIT setup; on TXM iPads that yields a JIT that faults. Reset once.
        if !defaults.bool(forKey: "settingsMigratedV2") {
            defaults.set(JITMethod.automatic.rawValue, forKey: "jitMethod")
            defaults.set(true, forKey: "settingsMigratedV2")
        }
        jitMethod = JITMethod(rawValue: defaults.integer(forKey: "jitMethod")) ?? .automatic
        useJIT = defaults.bool(forKey: "useJIT")
        jitMemoryMB = defaults.integer(forKey: "jitMemoryMB")
        verboseLog = defaults.bool(forKey: "verboseLog")
        consoleFontSize = defaults.double(forKey: "consoleFontSize")
        forceDarkMode = defaults.bool(forKey: "forceDarkMode")
    }
}
