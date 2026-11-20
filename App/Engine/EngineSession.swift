import Foundation
import SwiftUI

/// A Windows MessageBox request coming from the guest program.
struct MessageBoxRequest: Identifiable {
    struct Choice: Identifiable { let id: Int32; let title: String; let isCancel: Bool }
    let id = UUID()
    let title: String
    let text: String
    let choices: [Choice]

    init(title: String, text: String, type: UInt32) {
        self.title = title.isEmpty ? "Windows" : title
        self.text = text
        switch type & 0xF {
        case 1: choices = [.init(id: 1, title: "Tamam", isCancel: false), .init(id: 2, title: "İptal", isCancel: true)]
        case 2: choices = [.init(id: 3, title: "Durdur", isCancel: false), .init(id: 4, title: "Yeniden Dene", isCancel: false), .init(id: 5, title: "Yoksay", isCancel: false)]
        case 3: choices = [.init(id: 6, title: "Evet", isCancel: false), .init(id: 7, title: "Hayır", isCancel: false), .init(id: 2, title: "İptal", isCancel: true)]
        case 4: choices = [.init(id: 6, title: "Evet", isCancel: false), .init(id: 7, title: "Hayır", isCancel: false)]
        case 5: choices = [.init(id: 4, title: "Yeniden Dene", isCancel: false), .init(id: 2, title: "İptal", isCancel: true)]
        default: choices = [.init(id: 1, title: "Tamam", isCancel: false)]
        }
    }
}

private let soulOutputCallback: SoulOutputFn = { ctx, stream, data, len in
    guard let ctx, let data, len > 0 else { return }
    let session = Unmanaged<EngineSession>.fromOpaque(ctx).takeUnretainedValue()
    let bytes = UnsafeRawBufferPointer(start: UnsafeRawPointer(data), count: len)
    session.enqueueOutput(kind: stream, bytes: Array(bytes))
}

private let soulMessageBoxCallback: SoulMessageBoxFn = { ctx, title, text, type in
    guard let ctx else { return 1 }
    let session = Unmanaged<EngineSession>.fromOpaque(ctx).takeUnretainedValue()
    return session.presentMessageBox(title: title.map { String(cString: $0) } ?? "",
                                      text: text.map { String(cString: $0) } ?? "",
                                      type: type)
}

/// Runs one Windows program on a dedicated thread and exposes its console to SwiftUI.
final class EngineSession: ObservableObject, Identifiable {
    enum Phase: Equatable {
        case starting
        case running
        case finished(code: Int32)
        case failed(message: String)
    }

    let id = UUID()
    let program: WinProgram
    let startedAt = Date()

    @Published private(set) var phase: Phase = .starting
    @Published private(set) var lines: [ConsoleLine] = []
    @Published private(set) var revision = 0
    @Published private(set) var stats = SoulStats()
    @Published private(set) var jitEnabled = false
    @Published var messageBox: MessageBoxRequest?

    private let console = ConsoleModel()
    private let lock = NSLock()
    private var process: OpaquePointer?
    private var pending: [(Int32, [UInt8])] = []
    private var partial: [Int32: [UInt8]] = [:]
    private var flushScheduled = false
    private var statsTimer: Timer?
    private var boxSemaphore: DispatchSemaphore?
    private var boxResult: Int32 = 1

    init(program: WinProgram) {
        self.program = program
    }

    var isRunning: Bool {
        switch phase {
        case .starting, .running: return true
        default: return false
        }
    }

    var consoleText: String { console.plainText }

    // MARK: lifecycle

    func start() {
        SoulPaths.ensure()
        let settings = AppSettings.shared
        let jit = JITManager.shared
        let useJIT = settings.useJIT && jit.isAvailable
        jitEnabled = useJIT

        let exe = strdup(program.hostURL.path)
        let drive = strdup(SoulPaths.driveC.path)
        let win = strdup(program.windowsPath)
        let args = strdup(program.arguments)
        defer { free(exe); free(drive); free(win); free(args) }

        var opts = SoulLaunchOptions()
        opts.exe_path = UnsafePointer(exe)
        opts.drive_c = UnsafePointer(drive)
        opts.exe_win_path = UnsafePointer(win)
        opts.args = UnsafePointer(args)
        opts.jit_enabled = useJIT ? 1 : 0
        opts.verbose = settings.verboseLog ? 1 : 0
        opts.ctx = Unmanaged.passUnretained(self).toOpaque()
        opts.output = soulOutputCallback
        opts.message_box = soulMessageBoxCallback

        var err = [CChar](repeating: 0, count: 1024)
        var proc: OpaquePointer?
        let rc = soul_process_create(&opts, &proc, &err, err.count)
        guard rc == 0, let proc else {
            let message = String(cString: err)
            appendLocal("Program başlatılamadı: \(message)\n", kind: 2)
            phase = .failed(message: message)
            return
        }
        process = proc
        phase = .running
        let mode = useJIT ? "JIT (x86-64 → ARM64)" : "yorumlayıcı"
        appendLocal("▶︎ \(program.windowsPath) — \(mode)\n", kind: 3)

        let thread = Thread { [self] in
            let code = soul_process_run(proc)
            var final = SoulStats()
            soul_process_stats(proc, &final)
            let error = String(cString: soul_process_error(proc))
            lock.lock()
            process = nil
            soul_process_destroy(proc)
            lock.unlock()
            DispatchQueue.main.async { self.finish(code: code, error: error, stats: final) }
        }
        thread.name = "Soul.Guest"
        thread.stackSize = 16 << 20
        thread.qualityOfService = .userInitiated
        thread.start()

        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refreshStats()
        }
    }

    private func finish(code: Int32, error: String, stats final: SoulStats) {
        flushOutput()
        statsTimer?.invalidate()
        statsTimer = nil
        stats = final
        if !error.isEmpty && code == -1 {
            phase = .failed(message: error)
        } else {
            phase = .finished(code: code)
            appendLocal("\n■ Program sonlandı (çıkış kodu \(code)).\n", kind: 3)
        }
        if let box = messageBox { _ = box; messageBox = nil }
    }

    func stop() {
        lock.lock()
        if let p = process { soul_process_terminate(p) }
        lock.unlock()
        if messageBox != nil { answerMessageBox(2) }
    }

    func send(_ text: String) {
        let line = text + "\n"
        appendLocal(line, kind: 4)
        lock.lock()
        if let p = process { line.withCString { soul_process_send_input(p, $0, strlen($0)) } }
        lock.unlock()
    }

    func sendEOF() {
        appendLocal("^Z\n", kind: 4)
        lock.lock()
        if let p = process { soul_process_close_input(p) }
        lock.unlock()
    }

    private func refreshStats() {
        lock.lock()
        if let p = process {
            var s = SoulStats()
            soul_process_stats(p, &s)
            lock.unlock()
            stats = s
        } else {
            lock.unlock()
        }
    }

    // MARK: message boxes (called on the guest thread)

    func presentMessageBox(title: String, text: String, type: UInt32) -> Int32 {
        let sem = DispatchSemaphore(value: 0)
        lock.lock()
        boxSemaphore = sem
        boxResult = 1
        lock.unlock()
        DispatchQueue.main.async {
            self.flushOutput()
            self.messageBox = MessageBoxRequest(title: title, text: text, type: type)
        }
        sem.wait()
        lock.lock()
        let r = boxResult
        lock.unlock()
        return r
    }

    func answerMessageBox(_ value: Int32) {
        lock.lock()
        boxResult = value
        let sem = boxSemaphore
        boxSemaphore = nil
        lock.unlock()
        messageBox = nil
        sem?.signal()
    }

    // MARK: console output (called on the guest thread)

    func enqueueOutput(kind: Int32, bytes: [UInt8]) {
        lock.lock()
        pending.append((kind, bytes))
        let schedule = !flushScheduled
        flushScheduled = true
        lock.unlock()
        if schedule {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.04) { self.flushOutput() }
        }
    }

    private func flushOutput() {
        lock.lock()
        let chunks = pending
        pending.removeAll()
        flushScheduled = false
        lock.unlock()
        guard !chunks.isEmpty else { return }
        for (kind, bytes) in chunks {
            var data = (partial[kind] ?? []) + bytes
            let cut = Self.completeUTF8Prefix(data)
            partial[kind] = Array(data[cut...])
            data.removeSubrange(cut...)
            console.append(String(decoding: data, as: UTF8.self), kind: kind)
        }
        publish()
    }

    private func appendLocal(_ text: String, kind: Int32) {
        console.append(text, kind: kind)
        publish()
    }

    private func publish() {
        lines = console.lines
        revision &+= 1
    }

    /// Length of the longest prefix that does not end in the middle of a UTF-8 sequence.
    private static func completeUTF8Prefix(_ b: [UInt8]) -> Int {
        var i = b.count
        var back = 0
        while i > 0 && back < 4 {
            let c = b[i - 1]
            if c & 0xC0 == 0x80 { i -= 1; back += 1; continue }
            if c & 0x80 == 0 { return b.count }
            let need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2
            return (back + 1 >= need) ? b.count : i - 1
        }
        return b.count
    }
}

/// Only one guest process can run at a time (identity mapped address space).
@MainActor
final class SessionManager: ObservableObject {
    static let shared = SessionManager()
    @Published private(set) var current: EngineSession?

    @discardableResult
    func launch(_ program: WinProgram) -> EngineSession {
        if let current, current.isRunning { return current }
        let session = EngineSession(program: program)
        current = session
        session.start()
        ProgramLibrary.shared.markRun(program)
        return session
    }

    func clear() {
        if let current, current.isRunning { return }
        current = nil
    }
}
