import SwiftUI

/// Soul: runs 64-bit Windows (x64) programs on Apple Silicon iPads by translating
/// x86-64 machine code to ARM64 at runtime (JIT) and emulating the Win32 API.
@main
struct SoulApp: App {
    @StateObject private var settings = AppSettings.shared
    @StateObject private var jit = JITManager.shared
    @StateObject private var library = ProgramLibrary.shared
    @StateObject private var sessions = SessionManager.shared
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Start watching for the debugger: StikDebug attaches after launch and its
        // iPadOS 26 script then waits for our "prepare region" request.
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
        .onChange(of: scenePhase) {
            // coming back from StikDebug: the debugger may have attached meanwhile
            if scenePhase == .active { jit.checkNow() }
        }
    }
}
