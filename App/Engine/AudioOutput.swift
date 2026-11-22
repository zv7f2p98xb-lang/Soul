import AVFoundation
import Foundation

/// Plays the sound of the running Windows programs (waveOut, PlaySound, Beep, DirectSound)
/// through the iPad's speakers: an AVAudioSourceNode pulls SoulCore's 44.1 kHz stereo mix.
final class AudioOutput {
    static let shared = AudioOutput()

    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private var users = 0
    private let queue = DispatchQueue(label: "Soul.Audio")
    private var observer: NSObjectProtocol?

    private init() {}

    /// Called when a program starts; the first user starts the audio engine.
    func acquire() {
        queue.async { [self] in
            users += 1
            if users == 1 { startEngine() }
        }
    }

    /// Called when a program ends; the engine stops with the last one.
    func release() {
        queue.async { [self] in
            users = max(0, users - 1)
            if users == 0 { stopEngine() }
        }
    }

    private func makeSource() -> AVAudioSourceNode? {
        guard let format = AVAudioFormat(standardFormatWithSampleRate: Double(SOUL_AUDIO_RATE), channels: 2) else { return nil }
        // the render callback runs on the real-time audio thread: no allocations there
        let scratch = UnsafeMutablePointer<Float>.allocate(capacity: 2 * 4096)
        return AVAudioSourceNode(format: format) { isSilence, _, frameCount, bufferList -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            var done = 0
            var any: Int32 = 0
            let total = Int(frameCount)
            while done < total {
                let n = min(4096, total - done)
                any |= soul_audio_render(scratch, Int32(n))
                for i in 0..<n {
                    left[done + i] = scratch[2 * i]
                    right[done + i] = scratch[2 * i + 1]
                }
                done += n
            }
            isSilence.pointee = ObjCBool(any == 0)
            return noErr
        }
    }

    private func startEngine() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setActive(true)
        if source == nil, let node = makeSource() {
            source = node
            engine.attach(node)
            engine.connect(node, to: engine.mainMixerNode, format: node.outputFormat(forBus: 0))
        }
        if observer == nil {
            // route changes (headphones, AirPods) stop the engine: start it again
            observer = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil) { [weak self] _ in
                self?.queue.async {
                    guard let self, self.users > 0 else { return }
                    try? self.engine.start()
                }
            }
        }
        do {
            try engine.start()
            soul_audio_set_host(1)
        } catch {
            soul_audio_set_host(0) // no output: SoulCore keeps the sounds' timing on its own
        }
    }

    private func stopEngine() {
        soul_audio_set_host(0)
        engine.pause()
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
    }
}
