#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// The preliminary beat detector used before model inference. The decoder may
/// supply arbitrary buffers; the native sensor and envelopes receive 75 Hz blocks.
final class NativeBeatLegacy {
    typealias Result = NativeBeatInfoAnalyzer.Result

    enum Failure: Error { case incompleteSource }

    let sampleRate: Int
    let sourceFrameCount: Int
    let adjust: NativeBeatAdjust
    let envelope: NativeBeatEnvelope
    private let sensor: NativeBeatSensor
    private let blockFrames: Int
    private var pending: [Int16] = []
    private var processed = 0
    private var completed = false
    private var result: Result?

    init(sampleRate: Int, sourceFrameCount: Int) {
        precondition([44_100, 48_000].contains(sampleRate) && sourceFrameCount >= 0)
        self.sampleRate = sampleRate
        self.sourceFrameCount = sourceFrameCount
        blockFrames = sampleRate / 75
        sensor = .init(sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        adjust = .init(sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        envelope = .init(sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        pending.reserveCapacity(blockFrames * 2)
    }

    func append(interleaved samples: [Int16], frameCount: Int) throws {
        precondition(frameCount >= 0 && frameCount <= samples.count / 2)
        guard !completed else { return }
        var cursor = 0
        while cursor < frameCount {
            try Task.checkCancellation()
            let count = min(blockFrames - pending.count / 2, frameCount - cursor)
            pending.append(contentsOf: samples[(cursor * 2)..<((cursor + count) * 2)])
            cursor += count
            let ready = pending.count / 2
            if ready == blockFrames || processed + ready >= sourceFrameCount + 1 {
                try flush()
                if completed { return }
            }
        }
    }

    func finish() throws -> Result {
        if let result { return result }
        if !completed, processed + pending.count / 2 >= sourceFrameCount { try flush() }
        guard completed || sourceFrameCount == 0 else { throw Failure.incompleteSource }
        try Task.checkCancellation()
        var value = try NativeBeatInfoAnalyzer.analyze(candidates: sensor.candidates, low: adjust.low,
            sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        envelope.analyze(globalBPM: value.globalBPM, beats: &value.beats, dynamic: false)
        result = value
        return value
    }

    private func flush() throws {
        let count = pending.count / 2
        guard count > 0 else { return }
        let mono = stride(from: 0, to: pending.count, by: 2).map {
            (Float(pending[$0]) + Float(pending[$0 + 1])) * (1 / 65536)
        }
        try sensor.append(normalizedMono: mono)
        envelope.append(interleaved: pending, frameCount: count)
        adjust.append(interleaved: pending, frameCount: count)
        processed += count
        pending.removeAll(keepingCapacity: true)
        completed = processed >= sourceFrameCount
    }
}
