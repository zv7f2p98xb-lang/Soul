import SwiftUI

/// Soul: runs 64-bit Windows (x64) programs on Apple Silicon iPads by translating
/// x86-64 machine code to ARM64 at runtime (JIT) and emulating the Win32 API.
@main
struct SoulApp: App {
    @StateObject private var settings = AppSettings.shared
    @StateObject private var jit = JITManager.shared
    @StateObject private var library = ProgramLibrary.shared
    @StateObject private var sessions = SessionManager.shared

    init() {
        // JIT memory must be prepared as early as possible: on iPadOS 26 StikDebug's
        // script is waiting for our "prepare region" request right after launch.
        JITManager.shared.bootstrap()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(settings)
                .environmentObject(jit)
                .environmentObject(library)
                .environmentObject(sessions)
                .onOpenURL { url in
                    library.importItems([url])
                }
                .preferredColorScheme(settings.forceDarkMode ? .dark : nil)
        }
    }
}
