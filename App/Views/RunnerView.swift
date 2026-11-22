import SwiftUI
import UIKit
import QuickLook

struct RunnerView: View {
    @EnvironmentObject private var sessions: SessionManager

    var body: some View {
        if let session = sessions.current {
            SessionScreen(session: session, gui: session.gui)
        } else {
            ContentUnavailableView {
                Label("Çalışan program yok", systemImage: "macwindow")
            } description: {
                Text("Kitaplıktan bir program seçip çalıştırın. Pencereli programların pencereleri, konsol programlarının çıktısı burada görünür.")
            }
            .navigationTitle("Program")
        }
    }
}

/// Windowed programs get the desktop view, console programs the terminal.
struct SessionScreen: View {
    @ObservedObject var session: EngineSession
    @ObservedObject var gui: GuiModel

    var body: some View {
        Group {
            if session.program.isGUI || gui.everHadWindow {
                GuiScreen(session: session, gui: gui)
            } else {
                ConsoleScreen(session: session)
            }
        }
        // documents the program opens (read-me files, help pages) are shown with Quick Look
        .quickLookPreview($session.previewURL)
    }
}

struct ConsoleScreen: View {
    @ObservedObject var session: EngineSession
    @EnvironmentObject private var settings: AppSettings
    @EnvironmentObject private var sessions: SessionManager
    @State private var input = ""
    @FocusState private var inputFocused: Bool
    @State private var follow = true

    var body: some View {
        VStack(spacing: 0) {
            StatsBar(session: session)
            Divider()
            ScrollViewReader { proxy in
                ScrollView([.vertical]) {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(session.lines) { line in
                            Text(line.attributed.characters.isEmpty ? AttributedString(" ") : line.attributed)
                                .font(.system(size: settings.consoleFontSize, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }
                    .textSelection(.enabled)
                    .padding(12)
                }
                .background(Color(red: 0.06, green: 0.06, blue: 0.08))
                .onChange(of: session.revision) {
                    if follow { proxy.scrollTo("bottom", anchor: .bottom) }
                }
                .onTapGesture { inputFocused = true }
            }
            Divider()
            inputBar
        }
        .navigationTitle(session.program.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button { UIPasteboard.general.string = session.consoleText } label: { Label("Kopyala", systemImage: "doc.on.doc") }
                Toggle(isOn: $follow) { Label("Otomatik kaydır", systemImage: "arrow.down.to.line") }
                if session.isRunning {
                    Button(role: .destructive) { session.stop() } label: { Label("Durdur", systemImage: "stop.fill") }
                        .tint(.red)
                } else {
                    Button { sessions.launch(session.program) } label: { Label("Yeniden çalıştır", systemImage: "arrow.clockwise") }
                }
            }
        }
        .messageBoxAlert(session: session)
        .onAppear { inputFocused = session.isRunning }
    }

    private var inputBar: some View {
        HStack(spacing: 10) {
            Image(systemName: "chevron.right").foregroundStyle(.secondary)
            TextField(session.isRunning ? "Programa girdi gönder (Enter)" : "Program çalışmıyor", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .font(.system(size: settings.consoleFontSize, design: .monospaced))
                .focused($inputFocused)
                .submitLabel(.send)
                .onSubmit(send)
                .disabled(!session.isRunning)
            Button("Gönder", action: send)
                .buttonStyle(.borderedProminent)
                .disabled(!session.isRunning)
            Menu {
                Button("Boş satır gönder") { session.send("") }
                Button("Girdi sonu (Ctrl+Z)") { session.sendEOF() }
            } label: {
                Image(systemName: "keyboard")
            }
            .disabled(!session.isRunning)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(.bar)
    }

    private func send() {
        guard session.isRunning else { return }
        session.send(input)
        input = ""
        inputFocused = true
    }
}

struct StatsBar: View {
    @ObservedObject var session: EngineSession

    private var totalInsns: Double { Double(session.stats.jit_insns) + Double(session.stats.interp_insns) }
    private var mips: Double { session.stats.seconds > 0 ? totalInsns / session.stats.seconds / 1_000_000 : 0 }
    private var jitShare: Double { totalInsns > 0 ? Double(session.stats.jit_insns) / totalInsns : 0 }

    var body: some View {
        HStack(spacing: 18) {
            statusChip
            Label(session.jitEnabled ? "JIT" : "Yorumlayıcı", systemImage: session.jitEnabled ? "bolt.fill" : "tortoise.fill")
                .foregroundStyle(session.jitEnabled ? .green : .orange)
            metric("Süre", String(format: "%.1f s", session.stats.seconds))
            metric("x86 komut", formatCount(totalInsns))
            metric("Hız", String(format: "%.0f MIPS", mips))
            if session.jitEnabled {
                metric("JIT oranı", String(format: "%.1f%%", jitShare * 100))
                metric("Bloklar", "\(session.stats.jit_blocks)")
            }
            metric("API", "\(session.stats.api_calls)")
            Spacer(minLength: 0)
        }
        .font(.caption)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.bar)
    }

    @ViewBuilder private var statusChip: some View {
        switch session.phase {
        case .starting, .running:
            Label("Çalışıyor", systemImage: "circle.fill").foregroundStyle(.green)
        case .finished(let code):
            Label("Bitti (\(code))", systemImage: "checkmark.circle.fill").foregroundStyle(code == 0 ? .blue : .orange)
        case .stopped:
            Label("Durduruldu", systemImage: "stop.circle.fill").foregroundStyle(.secondary)
        case .failed:
            Label("Hata", systemImage: "xmark.octagon.fill").foregroundStyle(.red)
        }
    }

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).foregroundStyle(.secondary).font(.caption2)
            Text(value).monospacedDigit().fontWeight(.semibold)
        }
    }

    private func formatCount(_ v: Double) -> String {
        if v >= 1e9 { return String(format: "%.2f G", v / 1e9) }
        if v >= 1e6 { return String(format: "%.1f M", v / 1e6) }
        if v >= 1e3 { return String(format: "%.1f K", v / 1e3) }
        return String(format: "%.0f", v)
    }
}
