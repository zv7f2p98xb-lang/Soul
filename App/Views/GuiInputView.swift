import SwiftUI
import UIKit

/// Covers the client area of one Windows window and turns touches, pointer, scroll and
/// keyboard events into Win32 mouse/keyboard input.
struct GuiInputSurface: UIViewRepresentable {
    let hwnd: UInt64
    let pixelSize: CGSize      // client size in Windows pixels
    let wantsText: Bool
    let isActive: Bool
    @ObservedObject var gui: GuiModel

    func makeUIView(context: Context) -> GuiInputView {
        let v = GuiInputView()
        v.gui = gui
        v.hwnd = hwnd
        return v
    }

    func updateUIView(_ v: GuiInputView, context: Context) {
        v.gui = gui
        v.hwnd = hwnd
        v.pixelSize = pixelSize
        v.rightClickMode = gui.rightClickMode
        v.setKeyboard(visible: gui.softKeyboard || wantsText)
        if isActive && v.focusTicket != gui.focusTicket {
            v.focusTicket = gui.focusTicket
            DispatchQueue.main.async { if v.window != nil { v.becomeFirstResponder() } }
        }
    }
}

final class GuiInputView: UIView, UIKeyInput {
    weak var gui: GuiModel?
    var hwnd: UInt64 = 0
    var pixelSize: CGSize = .zero
    var rightClickMode = false
    var focusTicket = -1

    private var keyboardVisible = false
    private let noKeyboard = UIView(frame: .zero)
    private var tracked: UITouch?
    private var downSent = false
    private var downButton: Int32 = 0
    private var startPoint: CGPoint = .zero
    private var holdTimer: Timer?
    private var scrollAccum: CGFloat = 0
    private var hscrollAccum: CGFloat = 0

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        addGestureRecognizer(hover)
        let pan = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        pan.minimumNumberOfTouches = 2
        pan.allowedScrollTypesMask = .all
        pan.cancelsTouchesInView = true
        addGestureRecognizer(pan)
    }

    required init?(coder: NSCoder) { fatalError() }

    // MARK: coordinates

    private func client(_ p: CGPoint) -> (Int32, Int32) {
        guard bounds.width > 0, bounds.height > 0 else { return (0, 0) }
        let x = p.x * pixelSize.width / bounds.width, y = p.y * pixelSize.height / bounds.height
        return (Int32(x.rounded(.down)), Int32(y.rounded(.down)))
    }

    private func mouse(_ type: SoulGuiEventType, _ p: CGPoint, button: Int32 = 0) {
        let (x, y) = client(p)
        gui?.send(hwnd, Int32(type.rawValue), x: x, y: y, data: button)
    }

    // MARK: touches
    // A finger press is sent when it starts to move (drag), when it lifts (tap) or after a
    // 0.45 s hold, which becomes a right click. Mouse and trackpad clicks are sent at once.

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if !isFirstResponder { becomeFirstResponder() }
        guard tracked == nil, let t = touches.first else { return }
        tracked = t
        startPoint = t.location(in: self)
        downSent = false
        if t.type == .indirectPointer {
            let secondary = event?.buttonMask.contains(.secondary) ?? false
            press(button: secondary ? 1 : 0)
            return
        }
        mouse(SOUL_GUI_MOUSE_MOVE, startPoint)
        if rightClickMode { press(button: 1); return }
        holdTimer?.invalidate()
        holdTimer = Timer.scheduledTimer(withTimeInterval: 0.45, repeats: false) { [weak self] _ in
            guard let self, self.tracked != nil, !self.downSent else { return }
            self.press(button: 1)
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        }
    }

    private func press(button: Int32) {
        holdTimer?.invalidate()
        downSent = true
        downButton = button
        mouse(SOUL_GUI_MOUSE_DOWN, startPoint, button: button)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = tracked, touches.contains(t) else { return }
        let p = t.location(in: self)
        if !downSent {
            if hypot(p.x - startPoint.x, p.y - startPoint.y) < 6 { return }
            press(button: 0)
        }
        mouse(SOUL_GUI_MOUSE_MOVE, p)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = tracked, touches.contains(t) else { return }
        let p = t.location(in: self)
        if !downSent { press(button: 0) }
        mouse(SOUL_GUI_MOUSE_UP, p, button: downButton)
        tracked = nil
        holdTimer?.invalidate()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = tracked, touches.contains(t) else { return }
        if downSent { mouse(SOUL_GUI_MOUSE_UP, t.location(in: self), button: downButton) }
        tracked = nil
        holdTimer?.invalidate()
    }

    @objc private func hovered(_ g: UIHoverGestureRecognizer) {
        if g.state == .began || g.state == .changed { mouse(SOUL_GUI_MOUSE_MOVE, g.location(in: self)) }
    }

    @objc private func scrolled(_ g: UIPanGestureRecognizer) {
        let d = g.translation(in: self)
        g.setTranslation(.zero, in: self)
        scrollAccum += d.y * 3
        hscrollAccum += d.x * 3
        let (x, y) = client(g.location(in: self))
        while abs(scrollAccum) >= 40 {
            let step: CGFloat = scrollAccum > 0 ? 40 : -40
            scrollAccum -= step
            gui?.send(hwnd, Int32(SOUL_GUI_MOUSE_WHEEL.rawValue), x: x, y: y, data: Int32(step) * 3)
        }
        while abs(hscrollAccum) >= 40 {
            let step: CGFloat = hscrollAccum > 0 ? 40 : -40
            hscrollAccum -= step
            gui?.send(hwnd, Int32(SOUL_GUI_MOUSE_WHEEL.rawValue), x: x, y: y, data: (Int32(-step) * 3 & 0xFFFF) | 0x10000)
        }
        if g.state == .ended || g.state == .cancelled { scrollAccum = 0; hscrollAccum = 0 }
    }

    // MARK: keyboard

    override var canBecomeFirstResponder: Bool { true }

    override func becomeFirstResponder() -> Bool {
        let ok = super.becomeFirstResponder()
        if ok { gui?.activate(hwnd) }
        return ok
    }

    func setKeyboard(visible: Bool) {
        guard visible != keyboardVisible else { return }
        keyboardVisible = visible
        if isFirstResponder { reloadInputViews() }
    }

    override var inputView: UIView? { keyboardVisible ? nil : noKeyboard }
    var keyboardType: UIKeyboardType = .default
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no

    var hasText: Bool { true }

    func insertText(_ text: String) {
        gui?.send(hwnd, Int32(SOUL_GUI_TEXT.rawValue), text: text)
    }

    func deleteBackward() {
        gui?.send(hwnd, Int32(SOUL_GUI_KEY.rawValue), x: 1, y: 0, data: 0x08, ch: 8)
        gui?.send(hwnd, Int32(SOUL_GUI_KEY.rawValue), x: 0, y: 0, data: 0x08)
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var unhandled = Set<UIPress>()
        for p in presses {
            guard let key = p.key, let vk = Self.virtualKey(key) else { unhandled.insert(p); continue }
            gui?.send(hwnd, Int32(SOUL_GUI_KEY.rawValue), x: 1, y: Self.modifiers(key), data: vk, ch: Self.character(key, vk: vk))
        }
        if !unhandled.isEmpty { super.pressesBegan(unhandled, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var unhandled = Set<UIPress>()
        for p in presses {
            guard let key = p.key, let vk = Self.virtualKey(key) else { unhandled.insert(p); continue }
            gui?.send(hwnd, Int32(SOUL_GUI_KEY.rawValue), x: 0, y: Self.modifiers(key), data: vk)
        }
        if !unhandled.isEmpty { super.pressesEnded(unhandled, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) { pressesEnded(presses, with: event) }

    private static func modifiers(_ key: UIKey) -> Int32 {
        var m: Int32 = 0
        if key.modifierFlags.contains(.shift) { m |= Int32(SOUL_MOD_SHIFT.rawValue) }
        if key.modifierFlags.contains(.control) || key.modifierFlags.contains(.command) { m |= Int32(SOUL_MOD_CTRL.rawValue) }
        if key.modifierFlags.contains(.alternate) { m |= Int32(SOUL_MOD_ALT.rawValue) }
        return m
    }

    private static func character(_ key: UIKey, vk: Int32) -> UInt32 {
        let ctrl = key.modifierFlags.contains(.control) || key.modifierFlags.contains(.command)
        if ctrl, vk >= 0x41, vk <= 0x5A { return UInt32(vk - 0x40) } // Ctrl+A = 1 ...
        switch vk {
        case 0x0D: return 13
        case 0x08: return 8
        case 0x09: return 9
        case 0x1B: return 27
        default: break
        }
        if ctrl || key.modifierFlags.contains(.alternate) && key.characters.isEmpty { return 0 }
        guard let u = key.characters.utf16.first, u >= 32, u != 127 else { return 0 }
        if (0xF700...0xF8FF).contains(u) { return 0 } // function key private-use codes
        return UInt32(u)
    }

    /// USB HID usage -> Windows virtual-key code.
    static func virtualKey(_ key: UIKey) -> Int32? {
        let u = key.keyCode.rawValue
        switch u {
        case 0x04...0x1D: return Int32(0x41 + (u - 0x04))            // A-Z
        case 0x1E...0x26: return Int32(0x31 + (u - 0x1E))            // 1-9
        case 0x27: return 0x30                                        // 0
        case 0x28: return 0x0D
        case 0x29: return 0x1B
        case 0x2A: return 0x08
        case 0x2B: return 0x09
        case 0x2C: return 0x20
        case 0x2D: return 0xBD
        case 0x2E: return 0xBB
        case 0x2F: return 0xDB
        case 0x30: return 0xDD
        case 0x31, 0x32: return 0xDC
        case 0x33: return 0xBA
        case 0x34: return 0xDE
        case 0x35: return 0xC0
        case 0x36: return 0xBC
        case 0x37: return 0xBE
        case 0x38: return 0xBF
        case 0x39: return 0x14
        case 0x3A...0x45: return Int32(0x70 + (u - 0x3A))            // F1-F12
        case 0x46: return 0x2C
        case 0x47: return 0x91
        case 0x48: return 0x13
        case 0x49: return 0x2D
        case 0x4A: return 0x24
        case 0x4B: return 0x21
        case 0x4C: return 0x2E
        case 0x4D: return 0x23
        case 0x4E: return 0x22
        case 0x4F: return 0x27
        case 0x50: return 0x25
        case 0x51: return 0x28
        case 0x52: return 0x26
        case 0x53: return 0x90
        case 0x54: return 0x6F
        case 0x55: return 0x6A
        case 0x56: return 0x6D
        case 0x57: return 0x6B
        case 0x58: return 0x0D
        case 0x59...0x61: return Int32(0x61 + (u - 0x59))            // keypad 1-9
        case 0x62: return 0x60
        case 0x63: return 0x6E
        case 0x64: return 0xE2
        case 0xE0, 0xE4: return 0x11
        case 0xE1, 0xE5: return 0x10
        case 0xE2, 0xE6: return 0x12
        case 0xE3, 0xE7: return 0x5B
        default: return nil
        }
    }
}
