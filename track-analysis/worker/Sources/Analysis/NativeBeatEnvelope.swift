#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Low-frequency envelope sampled once per 128 source frames. Unlike the tempo
/// adjustment envelopes, this stream remains signed after DC removal.
final class NativeBeatEnvelope {
    private let sampleRate: Int
    private let sourceFrameCount: Int
    private var processed = 0
    private var pending = 0
    private var firstFiltered = 0.0
    private var firstLow: NativeBeatAdjust.Filter
    private var secondLow: NativeBeatAdjust.Filter
    private var smoother: NativeBeatAdjust.Filter
    private var removeDC: HighPass
    private(set) var values: [Int16] = []

    init(sampleRate: Int, sourceFrameCount: Int) {
        self.sampleRate = sampleRate
        self.sourceFrameCount = sourceFrameCount
        let rate = Double(sampleRate)
        firstLow = .init(cutoff: 150, sampleRate: rate, highPass: false)
        secondLow = .init(cutoff: 150, sampleRate: rate, highPass: false)
        smoother = .init(cutoff: 5, sampleRate: rate / 128, highPass: false)
        removeDC = .init(cutoff: 0.5, sampleRate: rate / 128)
        values.reserveCapacity((sourceFrameCount + 127) / 128)
    }

    func append(interleaved samples: [Int16], frameCount: Int) {
        precondition(frameCount >= 0 && frameCount <= samples.count / 2)
        let count = min(frameCount, sourceFrameCount - processed)
        guard count > 0 else { return }
        for frame in 0..<count {
            let mono = (Double(samples[frame * 2 + 1]) + Double(samples[frame * 2])) * 0.5
            let low = secondLow.process(firstLow.process(mono))
            if pending == 0 { firstFiltered = low }
            pending += 1
            processed += 1
            if pending == 128 || processed == sourceFrameCount {
                let output = removeDC.process(smoother.process(abs(firstFiltered)))
                values.append(Int16(truncatingIfNeeded: Int(output)))
                pending = 0
            }
        }
    }

    private struct HighPass {
        let b0: Double
        let b1: Double
        let a1: Double
        var previousInput = 0.0
        var previousOutput = 0.0

        init(cutoff: Double, sampleRate: Double) {
            let angle = cutoff * 6.28318530717958 / sampleRate
            b0 = 2 / (angle + 2)
            b1 = -b0
            a1 = (angle - 2) / (angle + 2)
        }

        mutating func process(_ input: Double) -> Double {
            let feedforward = (input * b0).addingProduct(previousInput, b1)
            let output = feedforward.addingProduct(-previousOutput, a1)
            previousInput = input
            previousOutput = output
            return output
        }
    }
}

extension NativeBeatEnvelope {
    func analyze(globalBPM: Float, beats: inout [NativeBeatInfo], dynamic: Bool) {
        Self.analyze(values: values, sampleRate: sampleRate, sourceFrameCount: sourceFrameCount,
                     globalBPM: globalBPM, beats: &beats, dynamic: dynamic)
    }

    /// The bar-phase analysis consumes the signed envelope after its final block.
    /// Beat periods are Float milliseconds; envelope positions are rounded Ints.
    static func analyze(values: [Int16], sampleRate: Int, sourceFrameCount: Int,
                        globalBPM: Float, beats: inout [NativeBeatInfo], dynamic: Bool) {
        guard globalBPM > 0, beats.count > 1 else { return }
        precondition(values.count == (sourceFrameCount + 127) / 128 && values.count > 1)
        let rate = Double(sampleRate)
        let firstPosition = Double(beats[0].sampleIndex) / 128
        let openingLimit = ((rate * 0.0009000000000000001)
                            * Double(beats[0].periodMilliseconds)) / 128
        let hasOpeningBeat = firstPosition > openingLimit
        var positions: [Int] = []
        var periods: [Double] = []
        if hasOpeningBeat {
            positions.append(1)
            periods.append((Double(beats[0].periodMilliseconds) * 0.001 * rate) / 128)
        }
        for beat in beats.prefix(beats.count - (hasOpeningBeat ? 1 : 0)) {
            positions.append(Int(Double(beat.sampleIndex) / 128 + 0.5))
            periods.append((Double(beat.periodMilliseconds) * 0.001 * rate) / 128)
        }
        positions[0] = max(1, positions[0])
        let unshiftedFirst = positions[0]
        let (amplitudes, peaks) = salientPeaks(values)
        // The original routine requires a completed envelope with a peak. A
        // silent completed envelope supplies all positions through its zero mean.
        precondition(!peaks.isEmpty)

        var longGapPeaks: [Int] = []
        var syncopatedPeaks: [Int] = []
        var syncopationScore = 0
        if peaks.count >= 3 {
            for index in 0..<(peaks.count - 2) {
                let peak = peaks[index]
                let firstGap = peaks[index + 1] - peak
                let secondGap = peaks[index + 2] - peaks[index + 1]
                let period = periods[floorIndex(peak, positions)]
                if Int(period * 2.25) < firstGap, firstGap <= Int(period * 2.75),
                   (Int(period * 1.35) < secondGap && secondGap <= Int(period * 1.6500000000000001)) ||
                    (Int(period * 1.8) < secondGap && secondGap <= Int(period * 2.2)) {
                    longGapPeaks.append(peak)
                }
                if Int(period * 1.35) < firstGap, firstGap <= Int(period * 1.6500000000000001) {
                    syncopationScore += 1
                    syncopatedPeaks.append(peak)
                } else if Int(period * 0.9) < firstGap, firstGap <= Int(period * 1.1),
                          Int(period * 0.9) < secondGap, secondGap <= Int(period * 1.1) {
                    syncopationScore -= 1
                }
            }
        }
        let searchOffset = Int(0.5.addingProduct(rate * 0.11, 1.0 / 128))
        let patternPeaks = longGapPeaks.count >= 11 ? longGapPeaks :
            syncopationScore >= 81 ? syncopatedPeaks : []
        var shifted = false
        if !dynamic {
            let globalPeriod = (rate * 60) / (128 * Double(globalBPM))
            let selected = patternPeaks.isEmpty ? peaks : patternPeaks
            let offbeatCount = selected.reduce(0) { count, peak in
                let target = peak - searchOffset
                return count + (abs(positions[nearestIndex(target, positions)] - target) > Int(globalPeriod * 0.25) ? 1 : 0)
            }
            if selected.count / 2 < offbeatCount {
                let shift = Int(globalPeriod * 0.5 + 0.5)
                let signedShift = Double(unshiftedFirst) <= globalPeriod * 0.45 + 0.5 ? shift : -shift
                for index in positions.indices { positions[index] += signedShift }
                positions[0] = max(1, positions[0])
                shifted = true
            }
        }
        var phaseScores = [Double](repeating: 0, count: 4)
        let firstPeak = peaks[0]
        let firstPeriod = periods[floorIndex(firstPeak, positions)]
        let firstGap = peaks.count > 1 ? peaks[1] - firstPeak : 0
        let secondSpan = peaks.count > 2 ? peaks[2] - firstPeak : 0
        let firstLower = Int(0.5.addingProduct(firstPeriod, 0.9))
        let firstUpper = Int(0.5.addingProduct(firstPeriod, 1.1))
        if (firstLower <= firstGap && firstGap <= firstUpper) ||
            (Int(0.5.addingProduct(firstPeriod, 1.8)) <= firstGap && firstGap <= Int(0.5.addingProduct(firstPeriod, 2.2))) ||
            (firstLower <= secondSpan && secondSpan <= firstUpper) {
            let phase = nearestIndex(firstPeak - min(searchOffset, firstPeak), positions) & 3
            phaseScores[phase] = Double(amplitudes[firstPeak]) * 2
        }
        let maximum = Double(peaks.map { amplitudes[$0] }.max()!)
        if peaks.count >= 3 {
            for index in 1..<(peaks.count - 1) {
                let peak = peaks[index]
                let period = periods[floorIndex(peak, positions)]
                if period * 7 < Double(peak) - Double(peaks[index - 1]) {
                    let gap = peaks[index + 1] - peak
                    let amplitude = Double(amplitudes[peak])
                    if (Int(0.5.addingProduct(period, 1.8)) <= gap && gap <= Int(0.5.addingProduct(period, 2.2))) ||
                        maximum * 0.6 < amplitude {
                        phaseScores[nearestIndex(peak - searchOffset, positions) & 3] += amplitude
                    }
                }
            }
        }
        for peak in patternPeaks {
            phaseScores[nearestIndex(peak - searchOffset, positions) & 3] += 983.04
        }
        var selectedPhase = 0
        for phase in 1..<4 where phaseScores[selectedPhase] < phaseScores[phase] { selectedPhase = phase }
        if phaseScores[selectedPhase] == 0 { selectedPhase = nearestIndex(firstPeak, positions) & 3 }
        let phaseBase = hasOpeningBeat ? (1 - selectedPhase) & 3 : 4 - selectedPhase
        let displacement = shifted ? positions[hasOpeningBeat ? 2 : 1] * 128 - beats[1].sampleIndex : 0
        for index in beats.indices {
            beats[index].sampleIndex += displacement
            beats[index].beatInBar = ((phaseBase + index) & 3) + 1
        }
        if beats[beats.count - 1].sampleIndex >= sourceFrameCount {
            beats.removeLast()
        } else {
            var last = beats[beats.count - 1]
            last.sampleIndex += Int(0.5.addingProduct(Double(last.periodMilliseconds), rate * 0.001))
            if last.sampleIndex < sourceFrameCount {
                last.beatInBar = last.beatInBar % 4 + 1
                beats.append(last)
            }
        }
        if beats[0].sampleIndex < 0 {
            beats.removeFirst()
        } else {
            var first = beats[0]
            let period = Int(0.5.addingProduct(rate * 0.001, Double(first.periodMilliseconds)))
            if period <= first.sampleIndex {
                first.sampleIndex -= period
                first.beatInBar = (first.beatInBar + 2) % 4 + 1
                beats.insert(first, at: 0)
            }
        }
    }

    private static func floorIndex(_ target: Int, _ positions: [Int]) -> Int {
        var lower = 0, upper = positions.count - 1
        while lower <= upper {
            let middle = lower + (upper - lower) / 2
            if positions[middle] <= target { lower = middle + 1 } else { upper = middle - 1 }
        }
        return max(0, upper)
    }

    private static func nearestIndex(_ target: Int, _ positions: [Int]) -> Int {
        var lower = 0, upper = positions.count - 1
        while lower <= upper {
            let middle = lower + (upper - lower) / 2
            let boundary = middle == 0 ? positions[0] : (positions[middle - 1] + positions[middle]) / 2
            if boundary <= target { lower = middle + 1 } else { upper = middle - 1 }
        }
        return max(0, upper)
    }

    private static func salientPeaks(_ values: [Int16]) -> (amplitudes: [Int16], positions: [Int]) {
        var amplitudes = [Int16](repeating: 0, count: values.count)
        var tracking = false, maximum: Int16 = 0, peakIndex = 0
        var total = 0, count = 0
        for index in 0..<(values.count - 1) {
            let next = values[index + 1]
            if !tracking, values[index] > next { maximum = next }
            let previousMaximum = maximum
            maximum = max(maximum, next)
            if previousMaximum < next { peakIndex = index + 1 }
            if tracking || previousMaximum < next {
                if next == 0 || Double(maximum) / Double(next) > 1.5 {
                    amplitudes[peakIndex] = maximum
                    if maximum > 0 { total += Int(maximum); count += 1 }
                    maximum = next
                    tracking = false
                } else { tracking = true }
            } else { tracking = false }
        }
        let mean = count == 0 ? 0 : Int16(total / count)
        var peaks: [Int] = []
        for index in 1..<values.count {
            if amplitudes[index] < mean { amplitudes[index] = 0 } else { peaks.append(index) }
        }
        return (amplitudes, peaks)
    }
}
