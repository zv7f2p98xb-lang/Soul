import SwiftUI

/// One rendered line of the emulated Windows console.
struct ConsoleLine: Identifiable, Equatable {
    let id: Int
    var spans: [ConsoleSpan] = []
    var attributed: AttributedString = AttributedString()

    static func == (a: ConsoleLine, b: ConsoleLine) -> Bool { a.id == b.id && a.spans == b.spans }
}

struct ConsoleStyle: Equatable {
    var fg: Int? = nil   // ANSI 0-15
    var bg: Int? = nil
    var bold = false
    var kind: Int32 = 1  // 1 stdout, 2 stderr, 3 Soul log, 4 echoed input
}

struct ConsoleSpan: Equatable {
    var text: String
    var style: ConsoleStyle
}

/// Converts the byte stream written by the guest (UTF-8 + ANSI SGR escapes that
/// SoulCore emits for SetConsoleTextAttribute & co.) into styled lines.
final class ConsoleModel {
    private(set) var lines: [ConsoleLine] = [ConsoleLine(id: 0)]
    private var nextID = 1
    private var style = ConsoleStyle()
    private var escape: String?
    private let maxLines = 5000

    static let palette: [Color] = [
        Color(red: 0.05, green: 0.05, blue: 0.05), Color(red: 0.80, green: 0.20, blue: 0.20),
        Color(red: 0.25, green: 0.70, blue: 0.30), Color(red: 0.80, green: 0.70, blue: 0.25),
        Color(red: 0.25, green: 0.45, blue: 0.85), Color(red: 0.70, green: 0.35, blue: 0.75),
        Color(red: 0.25, green: 0.70, blue: 0.75), Color(red: 0.80, green: 0.80, blue: 0.80),
        Color(red: 0.45, green: 0.45, blue: 0.45), Color(red: 1.00, green: 0.40, blue: 0.40),
        Color(red: 0.45, green: 0.95, blue: 0.45), Color(red: 1.00, green: 0.95, blue: 0.45),
        Color(red: 0.45, green: 0.65, blue: 1.00), Color(red: 0.95, green: 0.55, blue: 1.00),
        Color(red: 0.45, green: 0.95, blue: 1.00), Color(red: 1.00, green: 1.00, blue: 1.00),
    ]
    static let defaultForeground = Color(white: 0.9)

    var plainText: String {
        lines.map { $0.spans.map(\.text).joined() }.joined(separator: "\n")
    }

    func clear() {
        lines = [ConsoleLine(id: nextID)]
        nextID += 1
    }

    func append(_ text: String, kind: Int32) {
        var touched = Set<Int>()
        let base = style
        if kind != 1 { style = ConsoleStyle(kind: kind) }
        for ch in text {
            if var esc = escape {
                esc.append(ch)
                if ch.isLetter || esc.count > 24 {
                    handleEscape(esc)
                    escape = nil
                } else {
                    escape = esc
                }
                continue
            }
            switch ch {
            case "\u{1B}":
                escape = "\u{1B}"
            case "\n", "\r\n":
                newLine()
            case "\r":
                lines[lines.count - 1].spans.removeAll()
                touched.insert(lines.count - 1)
            case "\u{07}":
                break
            case "\u{08}":
                if var last = lines[lines.count - 1].spans.popLast() {
                    if !last.text.isEmpty { last.text.removeLast() }
                    if !last.text.isEmpty { lines[lines.count - 1].spans.append(last) }
                }
                touched.insert(lines.count - 1)
            case "\t":
                put("    ")
                touched.insert(lines.count - 1)
            default:
                put(String(ch))
                touched.insert(lines.count - 1)
            }
        }
        if kind != 1 { style = base }
        for i in touched where i < lines.count { render(i) }
        if lines.count > maxLines { lines.removeFirst(lines.count - maxLines) }
    }

    private func put(_ s: String) {
        let i = lines.count - 1
        if var last = lines[i].spans.last, last.style == style {
            last.text += s
            lines[i].spans[lines[i].spans.count - 1] = last
        } else {
            lines[i].spans.append(ConsoleSpan(text: s, style: style))
        }
    }

    private func newLine() {
        render(lines.count - 1)
        lines.append(ConsoleLine(id: nextID))
        nextID += 1
    }

    private func handleEscape(_ esc: String) {
        guard esc.hasPrefix("\u{1B}["), let final = esc.last else { return }
        let params = esc.dropFirst(2).dropLast().split(separator: ";").map { Int($0) ?? 0 }
        switch final {
        case "m":
            let list = params.isEmpty ? [0] : params
            for p in list {
                switch p {
                case 0: style.fg = nil; style.bg = nil; style.bold = false
                case 1: style.bold = true
                case 22: style.bold = false
                case 30...37: style.fg = p - 30
                case 90...97: style.fg = p - 90 + 8
                case 39: style.fg = nil
                case 40: style.bg = nil // black background == terminal background
                case 41...47: style.bg = p - 40
                case 100...107: style.bg = p - 100 + 8
                case 49: style.bg = nil
                default: break
                }
            }
        case "J":
            if params.first == 2 || params.first == 3 { clear() }
        case "K":
            lines[lines.count - 1].spans.removeAll()
        default:
            break // cursor movement is not emulated
        }
    }

    private func render(_ i: Int) {
        var out = AttributedString()
        for span in lines[i].spans {
            var a = AttributedString(span.text)
            switch span.style.kind {
            case 2: a.foregroundColor = Color(red: 1.0, green: 0.45, blue: 0.45)
            case 3: a.foregroundColor = Color(red: 0.95, green: 0.78, blue: 0.35)
            case 4: a.foregroundColor = Color(red: 0.55, green: 0.80, blue: 1.0)
            default:
                a.foregroundColor = span.style.fg.map { Self.palette[$0] } ?? Self.defaultForeground
                if let bg = span.style.bg { a.backgroundColor = Self.palette[bg] }
            }
            if span.style.bold { a.inlinePresentationIntent = .stronglyEmphasized }
            out += a
        }
        lines[i].attributed = out
    }
}
