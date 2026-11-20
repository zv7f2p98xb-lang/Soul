import SwiftUI

enum SidebarItem: Hashable {
    case library, console, jit, settings, about
}

struct ContentView: View {
    @EnvironmentObject private var sessions: SessionManager
    @EnvironmentObject private var jit: JITManager
    @State private var selection: SidebarItem? = .library
    @State private var columnVisibility = NavigationSplitViewVisibility.automatic

    var body: some View {
        NavigationSplitView(columnVisibility: $columnVisibility) {
            List(selection: $selection) {
                Section {
                    NavigationLink(value: SidebarItem.library) {
                        Label("Kitaplık", systemImage: "square.grid.2x2.fill")
                    }
                    NavigationLink(value: SidebarItem.console) {
                        HStack {
                            Label("Konsol", systemImage: "terminal.fill")
                            Spacer()
                            if sessions.current?.isRunning == true {
                                Circle().fill(.green).frame(width: 8, height: 8)
                            }
                        }
                    }
                }
                Section("Sistem") {
                    NavigationLink(value: SidebarItem.jit) {
                        HStack {
                            Label("JIT Durumu", systemImage: "bolt.fill")
                            Spacer()
                            Text(jit.isAvailable ? "Açık" : "Kapalı")
                                .font(.caption.weight(.semibold))
                                .padding(.horizontal, 8).padding(.vertical, 2)
                                .background(jit.isAvailable ? Color.green.opacity(0.2) : Color.orange.opacity(0.2), in: Capsule())
                                .foregroundStyle(jit.isAvailable ? .green : .orange)
                        }
                    }
                    NavigationLink(value: SidebarItem.settings) {
                        Label("Ayarlar", systemImage: "gearshape.fill")
                    }
                    NavigationLink(value: SidebarItem.about) {
                        Label("Nasıl Çalışır?", systemImage: "info.circle.fill")
                    }
                }
            }
            .navigationTitle("Soul")
        } detail: {
            NavigationStack {
                switch selection ?? .library {
                case .library: LibraryView()
                case .console: RunnerView()
                case .jit: JITStatusView()
                case .settings: SettingsView()
                case .about: AboutView()
                }
            }
        }
        .onReceive(sessions.$current) { session in
            if session != nil { selection = .console }
        }
        .task { sessions.startAutoRunIfRequested() }
    }
}
