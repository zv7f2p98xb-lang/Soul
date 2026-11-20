import SwiftUI
import UIKit

/// Shows the windows of a graphical Windows program like a small desktop.
struct GuiScreen: View {
    @ObservedObject var session: EngineSession
    @ObservedObject var gui: GuiModel
    @EnvironmentObject private var sessions: SessionManager
    @State private var showConsole = false

    var body: some View {
        VStack(spacing: 0) {
            StatsBar(session: session)
            Divider()
            GeometryReader { geo in
                DesktopView(session: session, gui: gui, size: geo.size)
            }
            .background(
                LinearGradient(colors: [Color(red: 0.11, green: 0.16, blue: 0.27), Color(red: 0.05, green: 0.33, blue: 0.45)],
                               startPoint: .topLeading, endPoint: .bottomTrailing)
            )
            .clipped()
        }
        .navigationTitle(session.program.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Toggle(isOn: $gui.softKeyboard) { Label("Klavye", systemImage: "keyboard") }
                    .help("Ekran klavyesini göster")
                Toggle(isOn: $gui.rightClickMode) { Label("Sağ tık", systemImage: "cursorarrow.click.2") }
                    .help("Dokunuşlar sağ tık olarak gönderilir (uzun basış da sağ tıktır)")
                Button { showConsole = true } label: { Label("Konsol çıktısı", systemImage: "terminal") }
                if session.isRunning {
                    Button(role: .destructive) { session.stop() } label: { Label("Durdur", systemImage: "stop.fill") }
                        .tint(.red)
                } else {
                    Button { sessions.launch(session.program) } label: { Label("Yeniden çalıştır", systemImage: "arrow.clockwise") }
                }
            }
        }
        .sheet(isPresented: $showConsole) {
            NavigationStack {
                ConsoleLog(session: session)
                    .navigationTitle("Konsol çıktısı")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar { Button("Kapat") { showConsole = false } }
            }
            .environmentObject(AppSettings.shared)
        }
        .messageBoxAlert(session: session)
    }
}

/// Placement of the windows inside the available area.
private struct DesktopLayout {
    let scale: CGFloat
    let origin: CGPoint      // view position of the desktop point `bounds.origin`
    let bounds: CGRect       // union of the window client rectangles (Windows pixels)

    static let titleHeight: CGFloat = 32
    static let menuHeight: CGFloat = 30

    static func chrome(_ w: GuiWindow) -> CGFloat {
        (w.hasCaption ? titleHeight : 0) + (w.hasMenu ? menuHeight : 0)
    }

    init(windows: [GuiWindow], size: CGSize) {
        var b = windows.first?.frame ?? CGRect(x: 0, y: 0, width: 640, height: 480)
        for w in windows { b = b.union(w.frame) }
        bounds = b
        let top = windows.map(Self.chrome).max() ?? 0
        let availW = max(100, size.width - 24), availH = max(100, size.height - 24 - top)
        scale = min(availW / max(1, b.width), availH / max(1, b.height), 1.75)
        origin = CGPoint(x: (size.width - b.width * scale) / 2, y: (size.height - top - b.height * scale) / 2 + top)
    }

    func clientRect(_ w: GuiWindow) -> CGRect {
        CGRect(x: origin.x + (w.frame.minX - bounds.minX) * scale, y: origin.y + (w.frame.minY - bounds.minY) * scale,
               width: w.frame.width * scale, height: w.frame.height * scale)
    }
}

private struct DesktopView: View {
    @ObservedObject var session: EngineSession
    @ObservedObject var gui: GuiModel
    let size: CGSize

    var body: some View {
        let windows = gui.windows
        if windows.isEmpty {
            VStack(spacing: 14) {
                if session.isRunning {
                    ProgressView()
                    Text(gui.everHadWindow ? "Pencere yok" : "Program başlatılıyor…").foregroundStyle(.white.opacity(0.8))
                } else {
                    Image(systemName: "macwindow.badge.plus").font(.system(size: 40)).foregroundStyle(.white.opacity(0.7))
                    Text("Program sonlandı").foregroundStyle(.white.opacity(0.8))
                }
            }
            .frame(width: size.width, height: size.height)
        } else {
            let layout = DesktopLayout(windows: windows, size: size)
            ZStack(alignment: .topLeading) {
                ForEach(windows) { w in
                    let r = layout.clientRect(w)
                    WindowFrame(window: w, gui: gui, scale: layout.scale, available: size)
                        .offset(x: r.minX, y: r.minY - DesktopLayout.chrome(w))
                }
            }
            .frame(width: size.width, height: size.height, alignment: .topLeading)
            .popover(item: Binding(get: { gui.popup }, set: { if $0 == nil { gui.answerPopup(0) } }),
                     attachmentAnchor: .rect(.rect(popupAnchor(layout))), arrowEdge: .top) { popup in
                PopupList(popup: popup) { gui.answerPopup($0) }
            }
        }
    }

    private func popupAnchor(_ layout: DesktopLayout) -> CGRect {
        guard let p = gui.popup, let w = gui.windows.first(where: { $0.id == p.hwnd }) else { return .zero }
        let r = layout.clientRect(w)
        return CGRect(x: r.minX + p.anchor.x * layout.scale, y: r.minY + p.anchor.y * layout.scale, width: 1, height: 1)
    }
}

/// Title bar, menu bar and client area of one window.
private struct WindowFrame: View {
    let window: GuiWindow
    @ObservedObject var gui: GuiModel
    let scale: CGFloat
    let available: CGSize

    var body: some View {
        let w = window.frame.width * scale, h = window.frame.height * scale
        VStack(spacing: 0) {
            if window.hasCaption { titleBar }
            if window.hasMenu { MenuBar(window: window, gui: gui) }
            ZStack(alignment: .topLeading) {
                if let img = window.image {
                    Image(decorative: img, scale: 1)
                        .resizable()
                        .interpolation(scale == 1 ? .none : .medium)
                        .frame(width: w, height: h)
                } else {
                    Color.white.frame(width: w, height: h)
                }
                GuiInputSurface(hwnd: window.id, pixelSize: window.frame.size, wantsText: window.wantsText, isActive: window.isActive, gui: gui)
                    .frame(width: w, height: h)
                if let c = window.caret, gui.caretVisible {
                    Rectangle()
                        .fill(Color.black)
                        .frame(width: max(1.5, c.width * scale * 0.75), height: c.height * scale)
                        .offset(x: c.minX * scale, y: c.minY * scale)
                        .allowsHitTesting(false)
                }
                if window.isDisabled {
                    Color.black.opacity(0.08).frame(width: w, height: h).allowsHitTesting(false)
                }
            }
            .frame(width: w, height: h)
        }
        .frame(width: w)
        .background(Color(uiColor: .systemBackground))
        .clipShape(UnevenRoundedRectangle(topLeadingRadius: window.hasCaption ? 10 : 0, bottomLeadingRadius: 3,
                                          bottomTrailingRadius: 3, topTrailingRadius: window.hasCaption ? 10 : 0))
        .shadow(color: .black.opacity(window.isActive ? 0.45 : 0.25), radius: window.isActive ? 18 : 8, y: 6)
    }

    private var titleBar: some View {
        HStack(spacing: 10) {
            Image(systemName: window.isDialog ? "rectangle.inset.filled" : "macwindow")
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(window.isActive ? Color.accentColor : .secondary)
            Text(window.title.isEmpty ? " " : window.title)
                .font(.system(size: 14, weight: .semibold))
                .lineLimit(1)
                .foregroundStyle(window.isActive ? Color.primary : Color.secondary)
            Spacer(minLength: 4)
            if window.isResizable {
                Button { fit() } label: {
                    Image(systemName: "arrow.up.left.and.arrow.down.right").font(.system(size: 12, weight: .bold))
                        .frame(width: 26, height: 22)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .accessibilityLabel("Ekrana sığdır")
            }
            if window.isClosable || window.hasCaption {
                Button { gui.close(window.id) } label: {
                    Image(systemName: "xmark").font(.system(size: 12, weight: .bold))
                        .frame(width: 30, height: 22)
                        .background(Color.red.opacity(window.isActive ? 0.85 : 0.35), in: RoundedRectangle(cornerRadius: 6))
                        .foregroundStyle(.white)
                }
                .buttonStyle(.plain)
                .disabled(window.isDisabled)
                .accessibilityLabel("Kapat")
            }
        }
        .padding(.horizontal, 10)
        .frame(height: DesktopLayout.titleHeight)
        .background(window.isActive ? Color(uiColor: .secondarySystemBackground) : Color(uiColor: .tertiarySystemBackground))
        .contentShape(Rectangle())
        .onTapGesture { gui.activate(window.id) }
    }

    /// Resizes the window so that it fills the visible area at 1 Windows pixel per point.
    private func fit() {
        let chrome = DesktopLayout.chrome(window)
        gui.resize(window.id, to: CGSize(width: available.width - 24, height: available.height - 24 - chrome))
    }
}

/// The program's menu bar shown as native iPad menus.
private struct MenuBar: View {
    let window: GuiWindow
    @ObservedObject var gui: GuiModel

    var body: some View {
        HStack(spacing: 2) {
            ForEach(gui.menus[window.id] ?? []) { item in
                if item.isSubmenu {
                    Menu {
                        MenuItems(items: item.children, hwnd: window.id, gui: gui)
                    } label: {
                        Text(item.label).font(.system(size: 14)).padding(.horizontal, 9).padding(.vertical, 4)
                    }
                    .disabled(item.isDisabled || window.isDisabled)
                    .simultaneousGesture(TapGesture().onEnded { gui.menuWillOpen(window.id) })
                } else if !item.isSeparator {
                    Button(item.label) { gui.command(window.id, item.command) }
                        .font(.system(size: 14))
                        .padding(.horizontal, 9).padding(.vertical, 4)
                        .disabled(item.isDisabled || window.isDisabled)
                }
            }
            Spacer(minLength: 0)
        }
        .foregroundStyle(.primary)
        .padding(.horizontal, 4)
        .frame(height: DesktopLayout.menuHeight)
        .background(Color(uiColor: .systemBackground))
        .overlay(alignment: .bottom) { Divider() }
    }
}

private struct MenuItems: View {
    let items: [GuiMenuItem]
    let hwnd: UInt64
    @ObservedObject var gui: GuiModel

    var body: some View {
        ForEach(items) { item in
            if item.isSeparator {
                Divider()
            } else if item.isSubmenu {
                Menu(item.label) { MenuItems(items: item.children, hwnd: hwnd, gui: gui) }
                    .disabled(item.isDisabled)
            } else {
                Button { gui.command(hwnd, item.command) } label: {
                    if item.isChecked {
                        Label(item.shortcut.map { "\(item.label)   \($0)" } ?? item.label, systemImage: "checkmark")
                    } else {
                        Text(item.shortcut.map { "\(item.label)   \($0)" } ?? item.label)
                    }
                }
                .disabled(item.isDisabled)
            }
        }
    }
}

/// Context menu (TrackPopupMenu) or combo box drop-down list.
private struct PopupList: View {
    let popup: GuiPopup
    let choose: (UInt32) -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                rows(popup.items, depth: 0)
            }
            .padding(.vertical, 6)
        }
        .frame(minWidth: 220, maxWidth: 360, maxHeight: 420)
        .presentationCompactAdaptation(.popover)
    }

    private func rows(_ items: [GuiMenuItem], depth: Int) -> AnyView {
        AnyView(ForEach(items) { item in
            if item.isSeparator {
                Divider().padding(.vertical, 4)
            } else if item.isSubmenu {
                Text(item.label).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                    .padding(.leading, 16 + CGFloat(depth) * 14).padding(.top, 8).padding(.bottom, 2)
                rows(item.children, depth: depth + 1)
            } else {
                Button { choose(item.command) } label: {
                    HStack {
                        Image(systemName: "checkmark").opacity(item.isChecked ? 1 : 0).font(.caption.weight(.bold))
                        Text(item.label).foregroundStyle(item.isDisabled ? Color.secondary : Color.primary)
                        Spacer()
                        if let s = item.shortcut { Text(s).font(.caption).foregroundStyle(.secondary) }
                    }
                    .padding(.leading, 12 + CGFloat(depth) * 14).padding(.trailing, 14).padding(.vertical, 9)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(item.isDisabled)
            }
        })
    }
}

/// Read-only console output of a GUI program (printf/stderr and Soul's log).
struct ConsoleLog: View {
    @ObservedObject var session: EngineSession
    @EnvironmentObject private var settings: AppSettings

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(session.lines) { line in
                    Text(line.attributed.characters.isEmpty ? AttributedString(" ") : line.attributed)
                        .font(.system(size: settings.consoleFontSize, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .textSelection(.enabled)
            .padding(12)
        }
        .background(Color(red: 0.06, green: 0.06, blue: 0.08))
    }
}

extension View {
    /// Presents the program's MessageBox calls as alerts.
    func messageBoxAlert(session: EngineSession) -> some View {
        modifier(MessageBoxAlert(session: session))
    }
}

private struct MessageBoxAlert: ViewModifier {
    @ObservedObject var session: EngineSession

    func body(content: Content) -> some View {
        content.alert(session.messageBox?.title ?? "", isPresented: Binding(get: { session.messageBox != nil }, set: { _ in }),
                      presenting: session.messageBox) { box in
            ForEach(box.choices) { choice in
                Button(choice.title, role: choice.isCancel ? .cancel : nil) { session.answerMessageBox(choice.id) }
            }
        } message: { box in
            Text(box.text)
        }
    }
}
