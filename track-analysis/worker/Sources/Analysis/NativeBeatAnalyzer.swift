#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativeBeatAnalyzer {
    struct Result: Sendable {
        let grid: NativeBeatGrid.Result
        let phraseSamples: NativePhraseFeatures.Samples
        let codecLeadingSeconds: Double
    }
    struct Stream {
        let sampleRate: Int
        let frameCount: Int
        let legacy: NativeBeatLegacy?
    }
    static func analyze(url: URL, backend: NativeBeatModel.Backend = .cpu,
                        rangeIndex: Int = 1, mode: RhythmMode = .automatic) throws -> Result {
        let decoder = try PCMDecoder(url: url)
        let phrase = NativePhrasePCMProcessor(sampleRate: Double(decoder.rate), channels: 2)
        let legacy = NativeBeatLegacy(sampleRate: decoder.rate, sourceFrameCount: decoder.frameCount - 1)
        var received = 0
        while let block = try decoder.read() {
            let count = block.count / 2
            try legacy.append(interleaved: block, frameCount: count)
            try phrase.append(interleaved: block, frameCount: count)
            received += count
        }
        guard received == decoder.frameCount else { throw AnalysisError.decodingFailed("Frame count changed") }
        let stream = Stream(sampleRate: decoder.rate, frameCount: received, legacy: legacy)
        let leading = 0.0
        var phraseSamples = phrase.finish(declaredFrameCount: stream.frameCount - 1)
        phraseSamples.leadingSeconds = leading
        func result(_ grid: NativeBeatGrid.Result = .init(beats: [], globalBPM100: 0,
                firstBeat: .init(offsetMilliseconds: -1, phase: -1))) -> Result {
            .init(grid: grid, phraseSamples: phraseSamples, codecLeadingSeconds: leading)
        }
        guard let legacy = stream.legacy else { return result() }
        let preliminary = try legacy.finish()
        // The original high-precision caller returns no grid when the
        // preliminary detector has no beats. It does not run the learned model.
        guard !preliminary.beats.isEmpty, preliminary.globalBPM > 0,
              preliminary.globalBPM.isFinite else { return result() }
        let levels = preliminary.beats.enumerated().map { index, beat in
            index == preliminary.beats.count - 1 ? 0 :
                legacy.adjust.lowIntegral(bpm: preliminary.globalBPM, sourceSampleIndex: beat.sampleIndex)
        }
        let offset = NativeBeatModelWindow.offset(lowIntegrals: levels,
            globalBPM: preliminary.globalBPM, sampleRate: stream.sampleRate)
        // Auto preserves the independently detected variation; Normal clears it.
        let dynamic = mode == .automatic && preliminary.detectedDynamic
        let plan = try NativeBeatInference.plan(sourceFrameCount: stream.frameCount,
            sampleRate: stream.sampleRate, requestedOffset: offset, dynamic: dynamic)
        let inferred = try NativeBeatInference.analyze(plan: plan, backend: backend) {
            try decoder.mono($0)
        }
        guard inferred.export.beats.count >= 4 else { return result() }
        let legacyBPM100 = Int(Float(0.5).addingProduct(preliminary.globalBPM, 100))
        var focused: NativeBeatFocus.Result
        if dynamic {
            focused = try NativeBeatFocusDynamic.analyze(model: inferred.export.beats,
                modelBPM100: inferred.export.globalBPM100, sampleRate: stream.sampleRate,
                sourceFrameCount: stream.frameCount - 1)
        } else {
            focused = try NativeBeatFocus.normal(model: inferred.export.beats,
                legacyBPM100: legacyBPM100, sampleRate: stream.sampleRate,
                sourceFrameCount: stream.frameCount - 1, windowOffset: inferred.windowOffset,
                lowIntegral: { legacy.adjust.lowIntegral(bpm: $0, sourceSampleIndex: $1) })
            // The native phase routine reads sixteen candidates from count+2
            // storage. Shorter outputs have no defined native phase result.
            guard focused.beats.count >= 14 else { return result() }
            try legacy.adjust.analyze(tempo: &focused.bpm, beats: &focused.beats)
            guard focused.beats.count >= 14 else { return result() }
            try NativeBeatFocusPhase.postprocess(beats: &focused.beats, sampleRate: stream.sampleRate,
                bpm: focused.bpm,
                lowIntegral: { legacy.adjust.lowIntegral(bpm: $0, sourceSampleIndex: $1) },
                fullIntegral: { legacy.adjust.fullIntegral(bpm: $0, sourceSampleIndex: $1) })
        }
        let grid = try NativeBeatGrid.convert(samples: focused.beats.map(\.sampleBeat),
            sampleRate: stream.sampleRate, sampleCount: stream.frameCount - 1,
            globalBPM100: Int(Float(0.5).addingProduct(focused.bpm, 100)), rangeIndex: rangeIndex, dynamic: dynamic)
        return result(grid)
    }

}
