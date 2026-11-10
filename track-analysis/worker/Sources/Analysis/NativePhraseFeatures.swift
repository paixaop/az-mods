#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Samples and coordinates used by the phrase analysis. Indices in the native
/// coordinate arrays are one-based; the Swift sample storage is zero-based.
enum NativePhraseFeatures {
    struct Samples: Sendable {
        let sourceRate: Double
        let sourceFrameCount: Int
        let low: [Double]
        let middle: [Double]
        let high: [Double]
        var leadingSeconds: Double = 0
    }

    struct Timing: Sendable, Equatable {
        let bpm: Double
        let offsetMilliseconds: Int
        let phase: Int
    }

    struct Coordinates: Sendable, Equatable {
        let beatCount: Int
        let high: [Int]
        let middle: [Int]
        let low: [Int]
    }

    struct Activity: Sendable, Equatable {
        let values: [Double]
        let changes: [UInt8]
    }

    struct AllActivity: Sendable, Equatable {
        let normalizedBeats: [Double]
        let bars: [Double]
    }

    struct Beats: Sendable {
        let timing: Timing
        let coordinates: Coordinates
        let initialBoundary: Int
        let lowLevels: [Double]
        let middleLevels: [Double]
        let allLevels: [Double]
        let lowPeaks: [Double]
        let lowPeakChanges: [Double]
        let lowQuarterPeaks: [Double]
        let lowQuarterMean: [Double]
        let middlePeaks: [Double]
        let highEighthPeaks: [Double]
        let lowActivity: Activity
        let middleActivity: Activity
        let allActivity: AllActivity
    }

    enum InputError: Error, Equatable {
        case invalidTiming
        case insufficientSamples
        case invalidSamples
    }

    /// The library supplies a single global tempo and the first detailed beat.
    /// The narrow offset adjustment is applied before either acoustic pass.
    static func referenceTiming(bpm: Double, firstBeatTime: Double, firstBeatPosition: Int?,
                                firstBeatTempoBPM: Double?) throws -> Timing {
        let bpm100 = (bpm * 100).rounded()
        guard bpm100.isFinite, bpm100 >= 1, bpm100 <= Double(Int32.max),
              firstBeatTime.isFinite, firstBeatTime >= 0,
              firstBeatTime * 1000 + 0.5 < Double(Int64.max) else { throw InputError.invalidTiming }
        if let firstBeatTempoBPM, !firstBeatTempoBPM.isFinite || firstBeatTempoBPM <= 0 {
            throw InputError.invalidTiming
        }
        var offset = Int(firstBeatTime * 1000 + 0.5)
        let positions = [1, 4, 3, 2]
        var phase = firstBeatPosition.flatMap { (1...4).contains($0) ? positions[$0 - 1] : nil } ?? 1
        if let firstBeatTempoBPM, (firstBeatTempoBPM * 100).rounded() != bpm100 {
            let period = 6_000_000 / bpm100
            if period < Double(offset), Double(offset) < period + period {
                offset -= Int(period)
                phase = phase == 1 ? 4 : phase - 1
            }
        }
        return Timing(bpm: bpm100 * 0.01, offsetMilliseconds: offset, phase: phase)
    }

    static func nominalBlockBeatCount(period: Double, phase: Int, offsetSeconds: Double) -> Int {
        let alignedStart = offsetSeconds.addingProduct(period, Double(phase - 1))
        return phase - 1 + 4 * Int((35.11 - alignedStart) / (period * 4))
    }

    /// Stage B builds a single stream's coordinates with a different operation
    /// order from the later three-stream lookup. Keep the two producers separate.
    static func stageBCoordinates(sampleCount: Int, rate: Double, bpm: Double,
                                   offsetMilliseconds: Int) -> [Int] {
        let period = (rate * 60) / bpm
        let offset = Int(0.5.addingProduct(rate * 0.001, Double(offsetMilliseconds)))
        let count = max(0, Int(Double(sampleCount - offset) / period))
        var result = (0..<count).map { Int((Double(offset) + 1.5).addingProduct(period, Double($0))) }
        if count > 0 { result[count - 1] = min(result[count - 1], sampleCount) }
        return result
    }

    static func coordinates(samples: Samples, timing: Timing) throws -> Coordinates {
        guard samples.sourceRate.isFinite, samples.sourceRate > 0,
              timing.bpm.isFinite, timing.bpm > 0, timing.offsetMilliseconds >= 0,
              (1...4).contains(timing.phase) else { throw InputError.invalidTiming }
        let rates = [samples.sourceRate / 2, samples.sourceRate / 8, samples.sourceRate / 32]
        let counts = [2.0, 8.0, 32.0].map { Double(samples.sourceFrameCount) / $0 }
        let seconds = Double(timing.offsetMilliseconds) * 0.001
        let offset = rates.map { Int(0.5.addingProduct(seconds, $0)) }
        let period = 60 / timing.bpm
        // The original rounds samples-per-beat once, then uses one fused
        // multiply-add for each position. Reassociation moves half-sample ties.
        let periods = rates.map { period * $0 }
        let count = zip(zip(counts, offset), periods).map { pair, period in
            Int((pair.0 - Double(pair.1)) / period)
        }.min() ?? 0
        guard count > 0 else { throw InputError.insufficientSamples }
        let indices = rates.indices.map { stream -> [Int] in
            var result = (0...count).map {
                Int((Double(offset[stream]) + 1.5).addingProduct(periods[stream], Double($0)))
            }
            // This clamp is on the last supported beat, not on its look-ahead.
            result[count - 1] = min(result[count - 1], Int(counts[stream]))
            return result
        }
        return Coordinates(beatCount: count, high: indices[0], middle: indices[1], low: indices[2])
    }

    /// The opening activity ordinal and the bar phase have different producers.
    static func activityStart(samples: [Double], rate: Double, bpm: Double, offsetMilliseconds: Int) -> Int {
        let period = bpm == 0 ? 0.5 : 60 / bpm
        let samplesPerBeat = Int(0.5.addingProduct(period, rate))
        let offset = Int(0.5.addingProduct(Double(offsetMilliseconds) * 0.001, rate))
        let length = min(samples.count, Int(rate * 30))
        var displacement = 0
        if length > 1 {
            for index in 0..<(length - 1) {
                let position = offset + index
                if position >= 0, position < length, abs(samples[position]) > 0.0005 {
                    displacement = index + 1
                    break
                }
            }
        }
        let ordinal = samplesPerBeat == 0
            ? Double(displacement) * 0.00036281179138321996
            : Double(displacement) / Double(samplesPerBeat)
        return Int(ordinal + 0.5) + 1
    }

    static func allActivity(_ source: [Double], phase: Int) -> AllActivity {
        let maximum = source.max() ?? 0
        let reciprocal = 1 / maximum
        let scale = reciprocal == 0 ? 0.003 : reciprocal
        let normalized = source.map { $0 * scale }
        let count = max(0, (source.count - phase + 4) / 4)
        var bars = [Double](repeating: 0, count: count)
        if source.count - phase + 4 >= 8 {
            for index in 0..<max(1, count - 1) {
                bars[index] = sum(normalized, start: phase - 1 + index * 4, count: 4) * 0.25
            }
        }
        return AllActivity(normalizedBeats: normalized, bars: bars)
    }

    static func lowActivity(_ source: [Double], phase: Int) -> Activity {
        let extent = source.count - phase + 4
        let count = max(0, extent / 4)
        var values = [Double](repeating: 0, count: count)
        var changes = [UInt8](repeating: 0, count: count)
        if extent >= 12 {
            let written = max(3, count) - 2
            for index in 0..<written {
                values[index] = sum(source, start: phase - 1 + index * 4, count: 4)
            }
            for index in 0..<written {
                let previous = values[index], next = values[index + 1]
                if (next > previous && next > previous * 1.2)
                    || (next < previous && previous > next * 1.2) {
                    changes[index + 1] = 1
                }
                // These two beat entries are deliberately independent of phase.
                if next <= 750, previous <= 750,
                   source[index * 4 + 4] <= 0.09, source[index * 4 + 3] <= 0.09 {
                    changes[index + 1] = 0
                }
            }
        }
        return Activity(values: values, changes: changes)
    }

    static func middleActivity(_ source: [Double], phase: Int) -> Activity {
        let extent = source.count - phase + 4
        let count = max(0, extent / 4)
        var values = [Double](repeating: 0, count: count)
        var changes = [UInt8](repeating: 0, count: count)
        if extent >= 8 {
            for index in 0..<max(1, count - 1) {
                values[index] = sum(source, start: phase - 1 + index * 4, count: 4)
            }
        }
        if extent >= 12 {
            for index in 1..<max(2, count - 1) {
                let next = values[index] == 0 ? 0.003 : values[index]
                let previous = values[index - 1] == 0 ? 0.003 : values[index - 1]
                if (next > previous && next / previous > 1.2)
                    || (previous > next && previous / next > 1.21) {
                    changes[index] = 1
                }
            }
        }
        return Activity(values: values, changes: changes)
    }

    static func middlePeakActivity(_ source: [Double], normalizationCount: Int, phase: Int) -> AllActivity {
        let maximum = source.max() ?? 0
        let scale = maximum == 0 ? 333.3333333333333 : 1 / maximum
        var normalized = source
        if normalizationCount > 1 {
            for index in 0..<min(source.count, normalizationCount - 1) { normalized[index] *= scale }
        }
        let extent = source.count - phase + 1
        let count = max(0, extent / 4)
        var bars = [Double](repeating: 0, count: count)
        if extent >= 8 {
            for index in 0..<max(1, count - 1) {
                bars[index] = sum(normalized, start: phase - 1 + index * 4, count: 4) * 0.25
            }
        }
        return AllActivity(normalizedBeats: normalized, bars: bars)
    }

    static func make(samples: Samples, timing: Timing) throws -> Beats {
        try Task.checkCancellation()
        let coordinates = try coordinates(samples: samples, timing: timing)
        guard (samples.low + samples.middle + samples.high).allSatisfy(\.isFinite) else {
            throw InputError.invalidSamples
        }
        let envelope = try NativePhraseEnvelopes.make(lowInput: samples.low, middleInput: samples.middle,
            highInput: samples.high, sourceRate: samples.sourceRate)
        let count = coordinates.beatCount
        let period = 60 / timing.bpm
        var lowLevels = [Double](repeating: 0, count: count)
        var middleLevels = lowLevels, allLevels = lowLevels, lowPeaks = lowLevels, middlePeaks = lowLevels
        var quarters = [Double](repeating: 0, count: count * 4)
        var eighths = [Double](repeating: 0, count: count * 8)
        // Analysis retains filter state but reduces approximately 35-second
        // blocks at beat boundaries. Each block's final low/high interval has
        // its own integer-period endpoint.
        var firstBeat = 0
        var blockStarts = Set<Int>()
        var first = true
        while firstBeat < count {
            try Task.checkCancellation()
            blockStarts.insert(firstBeat)
            let phase = first ? timing.phase : 1
            let offset = first ? Double(timing.offsetMilliseconds) * 0.001 : 0
            let nominal = nominalBlockBeatCount(period: period, phase: phase, offsetSeconds: offset)
            guard nominal > 0 else { throw InputError.invalidTiming }
            let lastBeat = min(count, firstBeat + nominal)
            let final = firstBeat + nominal > count
            let lowLimit = final ? samples.low.count : coordinates.low[lastBeat] - 1
            let middleLimit = final ? samples.middle.count : coordinates.middle[lastBeat] - 1
            let highLimit = final ? samples.high.count : coordinates.high[lastBeat] - 1
            for beat in firstBeat..<lastBeat {
                let lowStart = coordinates.low[beat]
                let naturalLowEnd = coordinates.low[beat + 1]
                let lowEnd = beat == lastBeat - 1 ? lowStart + Int(period * samples.sourceRate / 32) : naturalLowEnd
                let levelEnd = beat == lastBeat - 1 && lowEnd - 1 > lowLimit ? lowLimit : lowEnd
                lowLevels[beat] = sumInterval(envelope.lowLevel, start: lowStart, end: levelEnd, limit: lowLimit) * 4
                lowPeaks[beat] = maximumInterval(envelope.low, start: lowStart, end: lowEnd, limit: lowLimit)
                let quarter = Int(Double(lowEnd - lowStart) * 0.25 + 0.5)
                for part in 0..<4 {
                    let start = lowStart + part * quarter
                    let end = part == 3 ? lowEnd : start + quarter
                    quarters[beat * 4 + part] = maximumInterval(envelope.low, start: start, end: end, limit: lowLimit)
                }
                let middleStart = coordinates.middle[beat], middleEnd = coordinates.middle[beat + 1]
                middleLevels[beat] = sumInterval(envelope.middleLevel, start: middleStart, end: middleEnd, limit: middleLimit)
                middlePeaks[beat] = maximumInterval(envelope.middle, start: middleStart, end: middleEnd, limit: middleLimit)
                allLevels[beat] = maximumInterval(samples.middle, start: middleStart, end: middleEnd,
                    limit: middleLimit, absolute: true)
                let highStart = coordinates.high[beat]
                let highPeriod = period * samples.sourceRate / 2
                let proposedHighEnd = highStart + Int(highPeriod)
                let highEnd = beat == lastBeat - 1
                    ? (proposedHighEnd - 1 <= highLimit ? proposedHighEnd : highLimit) : coordinates.high[beat + 1]
                let eighth = Int(Double(Int(highPeriod + 0.5)) * 0.125 + 0.5)
                for part in 0..<8 {
                    let start = part == 0 ? highStart : highStart + part * eighth + 1
                    let end = part == 7 ? highEnd : highStart + (part + 1) * eighth + 1
                    eighths[beat * 8 + part] = maximumInterval(envelope.high, start: start, end: end, limit: highLimit)
                }
            }
            firstBeat = lastBeat
            first = false
        }
        var peakChanges = [Double](repeating: 0, count: count)
        for beat in 1..<count {
            let previous = lowPeaks[beat - 1], next = lowPeaks[beat]
            if blockStarts.contains(beat), previous == 0 { continue }
            if next > previous, next > previous * 1.77 { peakChanges[beat] = 1 }
            if next < previous, previous > next + next { peakChanges[beat] = 0.5 }
            if next <= 0.09, previous <= 0.09 { peakChanges[beat] *= 0.8 }
        }
        return Beats(timing: timing, coordinates: coordinates,
            initialBoundary: activityStart(samples: samples.middle, rate: samples.sourceRate / 8,
                bpm: timing.bpm, offsetMilliseconds: timing.offsetMilliseconds),
            lowLevels: lowLevels, middleLevels: middleLevels, allLevels: allLevels,
            lowPeaks: lowPeaks, lowPeakChanges: peakChanges, lowQuarterPeaks: quarters,
            lowQuarterMean: (0..<count).map { sum(quarters, start: $0 * 4, count: 4) * 0.25 },
            middlePeaks: middlePeaks, highEighthPeaks: eighths,
            lowActivity: lowActivity(lowLevels, phase: timing.phase),
            middleActivity: middleActivity(middleLevels, phase: timing.phase),
            allActivity: allActivity(allLevels, phase: timing.phase))
    }

    private static func sumInterval(_ source: [Double], start: Int, end: Int, limit: Int) -> Double {
        guard start > 0, start <= limit, end - 1 <= limit else { return 0 }
        return sum(source, start: start - 1, count: end - start)
    }

    private static func maximumInterval(_ source: [Double], start: Int, end: Int, limit: Int, absolute: Bool = false) -> Double {
        guard start > 0, start <= limit, end - 1 <= limit, end > start else { return 0 }
        let values = source[(start - 1)..<(end - 1)]
        return absolute ? values.map(abs).max() ?? 0 : values.max() ?? 0
    }

    /// The native array sum uses eight independent accumulators before its tail.
    static func sum(_ source: [Double], start: Int, count: Int) -> Double {
        guard start >= 0, count > 0, start + count <= source.count else { return 0 }
        var lanes = [Double](repeating: 0, count: 8)
        let vectorCount = count & ~7
        if vectorCount > 0 {
            for index in stride(from: 0, to: vectorCount, by: 8) {
                for lane in 0..<8 { lanes[lane] = source[start + index + lane] + lanes[lane] }
            }
        }
        var value = lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))
        value = value + (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])))
        for index in vectorCount..<count { value = source[start + index] + value }
        return value
    }
}

/// Source-rate filtering and decimation, with the original internal block size.
/// AVFoundation only decodes PCM; it does not resample these streams.
final class NativePhrasePCMProcessor {
    private struct Decimator {
        let divisor: Int
        var filters: [NativePhraseEnvelopes.Section]

        init(rate: Double, divisor: Int, highPass: Bool) {
            self.divisor = divisor
            let cutoff = (rate / Double(divisor)) * (highPass ? 0.55 : 0.4)
            filters = [1.9615705608064609, 1.6629392246050905, 1.1111404660392046, 0.39018064403225666].map {
                NativePhraseEnvelopes.secondOrder(rate: rate, angularCutoff: cutoff * 6.283185307179586,
                    damping: $0, highPass: highPass)
            }
        }

        mutating func process(_ input: [Double]) -> [Double] {
            var filtered = input
            for index in filters.indices { filtered = filters[index].process(filtered) }
            return stride(from: divisor - 1, to: (input.count / divisor) * divisor, by: divisor).map { filtered[$0] }
        }
    }

    private let rate: Double
    private let channels: Int
    private let blockSize: Int
    private var pending = [Double]()
    private var lowFilter: Decimator
    private var middleFilter: Decimator
    private var highFilter: Decimator
    private var low = [Double](), middle = [Double](), high = [Double]()
    private var frameCount = 0

    init(sampleRate: Double, channels: Int) {
        rate = sampleRate
        self.channels = channels
        blockSize = Int(sampleRate * 1.7066666666666668 + 0.5)
        lowFilter = Decimator(rate: sampleRate, divisor: 32, highPass: false)
        middleFilter = Decimator(rate: sampleRate, divisor: 8, highPass: false)
        highFilter = Decimator(rate: sampleRate, divisor: 2, highPass: true)
    }

    func append(interleaved: [Int16], frameCount count: Int) throws {
        guard channels == 2, interleaved.count >= count * 2 else { throw AnalysisError.unsupportedAudioFormat }
        pending += (0..<count).map {
            (Double(interleaved[$0 * 2]) * 3.021240234375e-05)
                .addingProduct(Double(interleaved[$0 * 2 + 1]), 3.0517578125e-07)
        }
        frameCount += count
        while pending.count >= blockSize {
            consume(Array(pending.prefix(blockSize)))
            pending.removeFirst(blockSize)
        }
    }

    private func consume(_ input: [Double]) {
        low += lowFilter.process(input)
        middle += middleFilter.process(input)
        high += highFilter.process(input)
    }

    func finish(declaredFrameCount: Int? = nil) -> NativePhraseFeatures.Samples {
        if !pending.isEmpty { consume(pending); pending.removeAll(keepingCapacity: false) }
        return .init(sourceRate: rate, sourceFrameCount: declaredFrameCount ?? frameCount, low: low, middle: middle, high: high)
    }
}
