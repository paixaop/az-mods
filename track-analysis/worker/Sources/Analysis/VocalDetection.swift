/* SPDX-License-Identifier: GPL-3.0-only */
#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif

/// Independent vocal timeline using the phrase detector's spectral heuristic.
/// A uniform clock covers the intro/tail and permits vocal-only analysis without
/// a beat grid. Scores are binary 0/4, not calibrated probabilities.
enum VocalDetection {
    static let hop = 1024, rate = 22050

    static func encode(samples: NativePhraseFeatures.Samples, sourceFrames: Int) throws -> Data {
        guard (samples.sourceRate == 44100 || samples.sourceRate == 48000), sourceFrames > 0,
              sourceFrames <= Int(samples.sourceRate) * 1200 else { throw WorkerError.outOfRange }
        let denominator = Int(samples.sourceRate) * hop
        let count = (sourceFrames * rate + denominator - 1) / denominator
        let transform = try NativePhraseSpectra.Transform(size: 2048)
        var measures = [NativePhraseSpectra.VocalMeasure]()
        measures.reserveCapacity(count)
        // The detector's frequency bins are calibrated at 5512.5 Hz. Resample
        // the filtered /8 stream explicitly, including the decimator's phase.
        let step = samples.sourceRate / 8 / 5512.5
        for frame in 0..<count {
            try Task.checkCancellation()
            let first = Double(frame * (hop / 4) + hop / 8 - 1024)
            var window = [Double](repeating: 0, count: 2048)
            for i in window.indices {
                let source = (first + Double(i)) * step - 0.875
                let left = Int(source.rounded(.down)), fraction = source - Double(left)
                if left >= 0 && left + 1 < samples.middle.count {
                    window[i] = samples.middle[left] * (1 - fraction) + samples.middle[left + 1] * fraction
                }
            }
            measures.append(try NativePhraseSpectra.vocalMeasure(transform.logMagnitude(window)))
        }
        let values = NativePhraseSpectra.vocalScores(measures).map { UInt8($0 > 0 ? 4 : 0) }
        var tag = Data("PVDI".utf8)
        tag.u32(24); tag.u32(24 + count); tag.u32(hop); tag.u16(rate); tag.u16(1); tag.u32(count)
        tag.append(contentsOf: values)
        return tag
    }
}
