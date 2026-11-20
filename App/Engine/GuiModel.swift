import Foundation
import SwiftUI
import UIKit
import QuartzCore

/// A visible top-level Windows window of the running program.
struct GuiWindow: Identifiable, Equatable {
    let id: UInt64             // HWND
    var owner: UInt64
    var frame: CGRect          // client area on the virtual desktop (Windows pixels)
    var flags: UInt32
    var title: String
    var image: CGImage?
    var version: UInt64
    var caret: CGRect?         // client coordinates

    var hasCaption: Bool { flags & UInt32(SOUL_WIN_CAPTION.rawValue) != 0 }
    var isResizable: Bool { flags & UInt32(SOUL_WIN_RESIZABLE.rawValue) != 0 }
    var isDialog: Bool { flags & UInt32(SOUL_WIN_DIALOG.rawValue) != 0 }
    var isActive: Bool { flags & UInt32(SOUL_WIN_ACTIVE.rawValue) != 0 }
    var isDisabled: Bool { flags & UInt32(SOUL_WIN_DISABLED.rawValue) != 0 }
    var wantsText: Bool { flags & UInt32(SOUL_WIN_WANTS_TEXT.rawValue) != 0 }
    var hasMenu: Bool { flags & UInt32(SOUL_WIN_HAS_MENU.rawValue) != 0 }
    var isMaximized: Bool { flags & UInt32(SOUL_WIN_MAXIMIZED.rawValue) != 0 }
    var isClosable: Bool { flags & UInt32(SOUL_WIN_CLOSABLE.rawValue) != 0 }

    static func == (a: GuiWindow, b: GuiWindow) -> Bool {
        a.id == b.id && a.version == b.version && a.frame == b.frame && a.flags == b.flags && a.title == b.title && a.caret == b.caret
    }
}

/// One entry of a Windows menu (menu bar, drop-down or popup menu).
final class GuiMenuItem: Identifiable {
    let id: Int
    let command: UInt32
    let title: String
    let flags: UInt32
    var children: [GuiMenuItem] = []

    init(id: Int, command: UInt32, title: String, flags: UInt32) {
        self.id = id
        self.command = command
        self.title = title
        self.flags = flags
    }

    var isSubmenu: Bool { flags & UInt32(SOUL_MENU_POPUP.rawValue) != 0 }
    var isSeparator: Bool { flags & UInt32(SOUL_MENU_SEPARATOR.rawValue) != 0 }
    var isChecked: Bool { flags & UInt32(SOUL_MENU_CHECKED.rawValue) != 0 }
    var isDisabled: Bool { flags & UInt32(SOUL_MENU_DISABLED.rawValue) != 0 }

    /// Menu text without the '&' access-key markers; the shortcut after a tab is split off.
    var label: String { Self.clean(title.components(separatedBy: "\t").first ?? title) }
    var shortcut: String? {
        let parts = title.components(separatedBy: "\t")
        return parts.count > 1 ? parts[1] : nil
    }
    static func clean(_ s: String) -> String {
        var out = ""
        var skip = false
        for ch in s {
            if ch == "&" && !skip { skip = true; continue }
            skip = false
            out.append(ch)
        }
        return out
    }

    static func tree(from entries: [SoulMenuEntry]) -> [GuiMenuItem] {
        var roots: [GuiMenuItem] = []
        var stack: [(Int32, GuiMenuItem)] = []
        for (i, e) in entries.enumerated() {
            let node = GuiMenuItem(id: i, command: e.id, title: soulString(e.text), flags: e.flags)
            while let last = stack.last, last.0 >= e.depth { stack.removeLast() }
            if let parent = stack.last { parent.1.children.append(node) } else { roots.append(node) }
            if node.isSubmenu { stack.append((e.depth, node)) }
        }
        return roots
    }
}

/// A popup menu (TrackPopupMenu) or combo box list the program wants the user to choose from.
struct GuiPopup: Identifiable, Equatable {
    let id = UUID()
    let hwnd: UInt64
    let isComboList: Bool
    let anchor: CGPoint        // client coordinates of hwnd
    let items: [GuiMenuItem]
    static func == (a: GuiPopup, b: GuiPopup) -> Bool { a.id == b.id }
}

/// Reads a fixed-size C char array (imported as a tuple) as a UTF-8 string.
func soulString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw -> String in
        guard let base = raw.baseAddress?.assumingMemoryBound(to: CChar.self) else { return "" }
        let len = strnlen(base, raw.count)
        return String(decoding: UnsafeRawBufferPointer(start: base, count: len), as: UTF8.self)
    }
}

/// Mirrors the program's windows into SwiftUI at display refresh rate and forwards input.
/// Used on the main thread only.
final class GuiModel: ObservableObject {
    @Published private(set) var windows: [GuiWindow] = []
    @Published private(set) var menus: [UInt64: [GuiMenuItem]] = [:]
    @Published var popup: GuiPopup?
    @Published private(set) var caretVisible = true
    /// Taps send right clicks (toggle in the toolbar).
    @Published var rightClickMode = false
    /// Show the on-screen keyboard even when no text field has the focus.
    @Published var softKeyboard = false
    @Published private(set) var everHadWindow = false
    /// Incremented when the active window should take the keyboard focus.
    @Published private(set) var focusTicket = 0

    weak var session: EngineSession?
    private var link: CADisplayLink?
    private var lastGeneration: UInt64 = .max
    private var lastActive: UInt64 = 0
    private var blinkPhase: CFTimeInterval = 0
    private var images: [UInt64: (version: UInt64, image: CGImage)] = [:]
    private var popupAnswered = false

    func start() {
        guard link == nil else { return }
        let l = CADisplayLink(target: DisplayLinkTarget(model: self), selector: #selector(DisplayLinkTarget.tick))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 60, preferred: 60)
        l.add(to: .main, forMode: .common)
        link = l
    }

    func stop() {
        link?.invalidate()
        link = nil
        popup = nil
    }

    private final class DisplayLinkTarget: NSObject {
        weak var model: GuiModel?
        init(model: GuiModel) { self.model = model }
        @objc func tick() { model?.poll() }
    }

    // MARK: polling

    private func poll() {
        guard let session else { return }
        var infos = [SoulWindowInfo](repeating: SoulWindowInfo(), count: 32)
        var result: [GuiWindow] = []
        var generation: UInt64 = 0
        var menuUpdate: [UInt64: [GuiMenuItem]]?
        var popupUpdate: GuiPopup??
        let alive = session.withProcess { p -> Bool in
            let n = Int(soul_gui_windows(p, &infos, Int32(infos.count)))
            generation = soul_gui_generation(p)
            for info in infos.prefix(n) {
                var w = GuiWindow(id: info.hwnd, owner: info.owner,
                                  frame: CGRect(x: Int(info.x), y: Int(info.y), width: Int(info.width), height: Int(info.height)),
                                  flags: info.flags, title: soulString(info.title), image: nil, version: info.version, caret: nil)
                if info.caret_visible != 0 {
                    w.caret = CGRect(x: Int(info.caret_x), y: Int(info.caret_y), width: max(2, Int(info.caret_w)), height: Int(info.caret_h))
                }
                if let cached = images[w.id], cached.version == w.version {
                    w.image = cached.image
                } else if let img = Self.copyImage(p, info) {
                    images[w.id] = (info.version, img)
                    w.image = img
                } else {
                    w.image = images[w.id]?.image
                }
                result.append(w)
            }
            if generation != lastGeneration {
                var m: [UInt64: [GuiMenuItem]] = [:]
                var entries = [SoulMenuEntry](repeating: SoulMenuEntry(), count: 256)
                for w in result where w.hasMenu {
                    let k = Int(soul_gui_menu(p, w.id, &entries, Int32(entries.count)))
                    m[w.id] = GuiMenuItem.tree(from: Array(entries.prefix(k)))
                }
                menuUpdate = m
                var info = SoulPopupInfo()
                let k = Int(soul_gui_popup(p, &info, &entries, Int32(entries.count)))
                if info.active != 0 {
                    if popup == nil {
                        let items = GuiMenuItem.tree(from: Array(entries.prefix(k)))
                        popupUpdate = .some(GuiPopup(hwnd: info.hwnd, isComboList: info.kind == 2,
                                                     anchor: CGPoint(x: Int(info.x), y: Int(info.y)), items: items))
                    }
                } else if popup != nil {
                    popupUpdate = .some(nil)
                }
            }
            return true
        }
        guard alive != nil else {
            if !windows.isEmpty { windows = [] }
            return
        }
        lastGeneration = generation
        let live = Set(result.map(\.id))
        images = images.filter { live.contains($0.key) }
        if result != windows { windows = result }
        if !result.isEmpty && !everHadWindow { everHadWindow = true }
        if let menuUpdate { menus = menuUpdate }
        if let popupUpdate {
            popupAnswered = false
            popup = popupUpdate
        }
        let active = result.last(where: { $0.isActive })?.id ?? 0
        if active != lastActive {
            lastActive = active
            focusTicket &+= 1
        }
        let now = CACurrentMediaTime()
        let on = Int((now - blinkPhase) / 0.53) % 2 == 0
        if on != caretVisible { caretVisible = on }
    }

    private static func copyImage(_ p: OpaquePointer, _ info: SoulWindowInfo) -> CGImage? {
        let w = Int(info.width), h = Int(info.height)
        guard w > 0, h > 0 else { return nil }
        let stride = w * 4
        var data = Data(count: stride * h)
        var version = UInt64.max
        let ok = data.withUnsafeMutableBytes { raw in
            soul_gui_copy(p, info.hwnd, raw.baseAddress, stride, Int32(w), Int32(h), &version)
        }
        guard ok != 0, let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: stride,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    // MARK: input

    func send(_ hwnd: UInt64, _ type: Int32, x: Int32 = 0, y: Int32 = 0, data: Int32 = 0, ch: UInt32 = 0, text: String? = nil) {
        guard let session else { return }
        session.withProcess { p in
            if let text { text.withCString { soul_gui_event(p, hwnd, type, x, y, data, ch, $0) } }
            else { soul_gui_event(p, hwnd, type, x, y, data, ch, nil) }
        }
        if type == Int32(SOUL_GUI_KEY.rawValue) || type == Int32(SOUL_GUI_TEXT.rawValue) || type == Int32(SOUL_GUI_MOUSE_DOWN.rawValue) {
            blinkPhase = CACurrentMediaTime()
        }
    }

    func command(_ hwnd: UInt64, _ id: UInt32) {
        send(hwnd, Int32(SOUL_GUI_COMMAND.rawValue), data: Int32(bitPattern: id))
        send(hwnd, Int32(SOUL_GUI_MENU_OPEN.rawValue))
    }

    func menuWillOpen(_ hwnd: UInt64) { send(hwnd, Int32(SOUL_GUI_MENU_OPEN.rawValue)) }
    func close(_ hwnd: UInt64) { send(hwnd, Int32(SOUL_GUI_CLOSE.rawValue)) }
    func activate(_ hwnd: UInt64) { send(hwnd, Int32(SOUL_GUI_ACTIVATE.rawValue)) }
    func resize(_ hwnd: UInt64, to size: CGSize) {
        send(hwnd, Int32(SOUL_GUI_RESIZE.rawValue), x: Int32(max(160, size.width)), y: Int32(max(120, size.height)))
    }

    func answerPopup(_ id: UInt32) {
        guard !popupAnswered else { return }
        popupAnswered = true
        send(0, Int32(SOUL_GUI_POPUP_RESULT.rawValue), data: Int32(bitPattern: id))
        popup = nil
    }

    /// Renders all visible windows into one image (used by automated test runs).
    func snapshot() -> UIImage? {
        guard !windows.isEmpty else { return nil }
        let bounds = windows.map(\.frame).reduce(windows[0].frame) { $0.union($1) }.insetBy(dx: -8, dy: -8)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        return UIGraphicsImageRenderer(size: bounds.size, format: format).image { ctx in
            UIColor(red: 0.16, green: 0.2, blue: 0.27, alpha: 1).setFill()
            ctx.fill(CGRect(origin: .zero, size: bounds.size))
            for w in windows {
                guard let img = w.image else { continue }
                let r = w.frame.offsetBy(dx: -bounds.minX, dy: -bounds.minY)
                UIImage(cgImage: img).draw(in: r)
            }
        }
    }
}
