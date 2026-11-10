#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseSpectra {
    struct Peak {
        let position: Int
        let height: Double
        let width: Double
        let prominence: Double
    }

    /// The peak position is one-based. A rising plateau emits its first sample.
    /// Prominence uses the higher of the two intervening minima; the decompiler
    /// drops the second minimum, while the original instructions retain it.
    static func peaks(_ samples: [Double]) -> [Peak] {
        guard samples.count > 1 else { return [Peak(position: 0, height: 0, width: 0, prominence: 0)] }
        var positions = [Int](), previous = 0
        for i in 0..<(samples.count - 1) {
            let difference = samples[i] - samples[i + 1]
            let direction = difference < 0 ? 1 : difference == 0 ? 0 : 2
            if difference >= 0 && previous == 1 { positions.append(i) }
            previous = direction
        }
        guard !positions.isEmpty else { return [Peak(position: 0, height: 0, width: 0, prominence: 0)] }
        return positions.map { position in
            let height = samples[position]
            var left = position, right = position
            while left > 0 && samples[left] <= height { left -= 1 }
            while right < samples.count - 1 && samples[right] <= height { right += 1 }
            let leftMinimum = samples[left...position].min()!
            let rightMinimum = samples[position...right].min()!
            let prominence = height - max(leftMinimum, rightMinimum)
            let half = height.addingProduct(prominence, -0.5)
            left = position; right = position
            while left > 0 && samples[left] >= half { left -= 1 }
            while right < samples.count - 1 && samples[right] >= half { right += 1 }
            let leftCrossing = Double(left + 1) + (half - samples[left]) / (samples[left + 1] - samples[left])
            let rightCrossing = Double(right) + (half - samples[right - 1]) / (samples[right] - samples[right - 1])
            return Peak(position: position + 1, height: height, width: rightCrossing - leftCrossing, prominence: prominence)
        }
    }

    static func vocalActivity(_ spectra: [[Double]]) throws -> [Double] {
        guard !spectra.isEmpty, spectra.allSatisfy({ $0.count >= 743 }) else { throw NativePhraseClassifier.InputError.inconsistentDimensions }
        var selected = [[Peak]](), levels = [Double]()
        for row in spectra {
            var section = NativePhraseEnvelopes.secondOrder(rate: 5512.5, angularCutoff: 471.23889803846896,
                damping: 1.4142135623730951, highPass: false)
            let values = section.process(row)
            selected.append(peaks(values).filter { $0.position >= 112 })
            var total = 0.0
            for i in 111..<743 { total += pow(10, values[i] * 0.05) }
            levels.append(log10(total * 0.0015822784810126582) * 20)
        }
        var total = 0.0
        for value in levels { total += pow(10, value * 0.05) }
        let reference = log10(total / Double(levels.count))
        return selected.map { candidates in
            max(0, candidates.filter { $0.height > reference * 38.8 && $0.width / Double($0.position) > 0.11 && $0.prominence > 10 }
                .map(\.height).max() ?? 0)
        }
    }

    static func vocalEntries(_ activity: [Double]) -> [Int] {
        guard !activity.isEmpty else { return [] }
        let maximum = activity.max()!
        let normalized = activity.map { $0 * (maximum == 0 ? 333.3333333333333 : 1 / maximum) }
        let limit = max(0, activity.count - 4)
        var mask = [Int](repeating: 0, count: activity.count)
        for i in 0..<limit { mask[i] = normalized[i] > 0 ? 1 : 0 }
        var blockSums = [Double](repeating: 0, count: activity.count / 4)
        if blockSums.count > 1 {
            for i in 0..<(blockSums.count - 1) { blockSums[i] = normalized[(i * 4)..<(i * 4 + 4)].reduce(0, +) }
        }
        var ratios = [Double](repeating: 0, count: blockSums.count)
        if blockSums.count > 2 {
            for i in 1..<(blockSums.count - 1) { ratios[i] = blockSums[i - 1] == 0 ? 0 : blockSums[i] / blockSums[i - 1] }
        }
        var entries = [Int](), silence = 0, armed = false
        for i in 0..<limit {
            if mask[i] == 0 {
                if silence > 6 { armed = true }
                silence += 1
            } else {
                if armed && mask[i...min(i + 2, mask.count - 1)].reduce(0, +) >= 2 { entries.append(i + 1) }
                armed = false; silence = 0
            }
        }
        let density = limit == 0 ? 0 : Double(mask.reduce(0, +)) / Double(limit)
        if activity.count > 8 && density < 0.6 {
            silence = 0; armed = false
            var longSilence = false
            for i in 0..<(activity.count - 8) {
                if mask[i] == 0 {
                    if silence > 6 { armed = false; longSilence = true }
                    else if silence > 1 { armed = true }
                    silence += 1
                } else {
                    if armed && !longSilence && mask[i...i + 2].reduce(0, +) > 2 && normalized[i] >= 0.7 {
                        let bar = i / 4
                        if ratios[bar] >= 1.8 || ratios[bar + 1] >= 3 { entries.append(i + 1) }
                    }
                    silence = 0; armed = false; longSilence = false
                }
            }
        }
        let sorted = Set(entries).sorted()
        return sorted.map { $0 / 4 + 1 }
    }

    /// FFT_calc_1 has one 2048-sample window per beat. FFT_calc_2 has a
    /// 4096-sample window on beats one and three of each complete bar.
    static func make(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                     coordinates: NativePhraseFeatures.Coordinates, recurrence: Bool) throws -> [[Double]] {
        var result = [[Double]]()
        for block in try NativePhraseRhythmFeatures.blocks(samples: samples, timing: timing, coordinates: coordinates) {
            try Task.checkCancellation()
            guard block.beatCount >= block.phase else { continue }
            let positions = coordinates.middle[block.firstBeat...block.endBeat].map { $0 - block.middle.lowerBound }
            result += try make(samples: Array(samples.middle[block.middle]), sampleRate: samples.sourceRate / 8,
                bpm: timing.bpm, beatSamples: positions, phase: block.phase, endBeat: block.beatCount, recurrence: recurrence)
        }
        return result
    }

    static func make(samples: [Double], sampleRate: Double, bpm: Double, beatSamples: [Int],
                     phase: Int, endBeat: Int, recurrence: Bool) throws -> [[Double]] {
        guard sampleRate > 0, bpm > 0, phase >= 1, endBeat >= phase, endBeat <= beatSamples.count,
              beatSamples == beatSamples.sorted(), beatSamples.first! >= 1 else { throw NativePhraseClassifier.InputError.unsupportedRange }
        let bars = (endBeat - phase + 1) / 4
        let size = recurrence ? 4096 : 2048
        let offsets = recurrence ? [0, 2] : [0, 1, 2, 3]
        let bufferCount = Int(ceil(sampleRate * 60 / bpm)) * 4 + size + 1
        let fft = try Transform(size: size)
        var result = [[Double]]()
        for bar in 0..<bars {
            try Task.checkCancellation()
            let beat = phase - 1 + bar * 4
            let start = beatSamples[beat] - 1
            let end = bar + 1 < bars ? min(samples.count, beatSamples[beat + 4] - 1) : min(samples.count, start + bufferCount)
            guard start >= 0, start < samples.count, end >= start else { throw NativePhraseClassifier.InputError.unsupportedRange }
            for offset in offsets {
                let position = beatSamples[beat + offset] - 1
                let availableEnd = min(end, position + size)
                var input = [Double](repeating: 0, count: size)
                if position < availableEnd {
                    input.replaceSubrange(0..<(availableEnd - position), with: samples[position..<availableEnd])
                }
                result.append(fft.logMagnitude(input))
            }
        }
        return result
    }

    private final class Transform {
        let size: Int
        let window: [Double]

        init(size: Int) throws {
            self.size = size
            let center = Double(size - 1) * 0.5
            let angularStep = Double.pi / (center + 1)
            window = (0..<size).map { 0.5.addingProduct(cos((Double($0) - center) * angularStep), 0.5) }
        }


        func logMagnitude(_ samples: [Double]) -> [Double] {
            var real = zip(samples, window).map(*)
            var imaginary = [Double](repeating: 0, count: size)
            ta_fft_d(&real, &imaginary, size)
            return (0..<(size / 2)).map { i in
                let magnitude = sqrt((real[i] * real[i]).addingProduct(imaginary[i], imaginary[i]))
                return magnitude < 1e-10 ? -200 : log10(magnitude) * 20
            }
        }
    }
}
