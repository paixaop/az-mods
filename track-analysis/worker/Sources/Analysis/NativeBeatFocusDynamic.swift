#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Repairs tempo-octave discontinuities in the model's variable-tempo grid.
enum NativeBeatFocusDynamic {
    struct Span: Codable, Equatable, Sendable {
        var first: Int
        var last: Int
    }

    struct Snapshot {
        let times: [Double]
        let intervals: [Double]
        let ratios: [Double]
        let symmetricRatios: [Double]
        let period: Double
        let bpm: Float
        let spans: [Span]
    }

    struct Prefix {
        var beats: [NativeBeatInfo]
        var anchorSample: Int
        var anchorBar: Int
        var bpm: Float
    }

    static func analyze(model: [NativeBeatExport.Beat], modelBPM100: Int,
                        sampleRate: Int, sourceFrameCount: Int) throws -> NativeBeatFocus.Result {
        let prepared = try prefix(model: model, modelBPM100: modelBPM100,
                                  sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        return try NativeBeatFocusDynamicGrid.finish(beats: prepared.beats, sampleRate: sampleRate,
            sourceFrameCount: sourceFrameCount, anchorSample: prepared.anchorSample, anchorBar: prepared.anchorBar)
    }

    static func prefix(model: [NativeBeatExport.Beat], modelBPM100: Int,
                       sampleRate: Int, sourceFrameCount: Int) throws -> Prefix {
        guard model.count >= 4, modelBPM100 > 0, sampleRate > 0, sourceFrameCount > 0,
              model.allSatisfy({ $0.pos >= 0 }) else { throw NativeBeatFocus.Failure.invalidInput }
        var previousPeriod: Float = 0
        var result = Prefix(beats: model.map { beat in
            if beat.bpm != 0 { previousPeriod = 6_000_000 / Float(beat.bpm) }
            return .init(sampleIndex: beat.pos, intervalInBeats: 0, periodMilliseconds: previousPeriod,
                         beatInBar: beat.numBar, sourceBand: -1, flag: 0)
        }, anchorSample: -1, anchorBar: 0, bpm: Float(modelBPM100) * 0.01)
        for _ in 0..<2 {
            try Task.checkCancellation()
            let state = try snapshot(beats: result.beats, sampleRate: sampleRate, modelBPM100: modelBPM100)
            result.bpm = state.bpm
            if result.anchorSample < 0 {
                let index = anchorIndex(count: result.beats.count, spans: state.spans)
                result.anchorSample = result.beats[index].sampleIndex
                result.anchorBar = result.beats[index].beatInBar
            }
            let ranges = state.spans.map { subranges(ratios: state.ratios, span: $0) }
            for index in state.spans.indices.reversed() {
                try Task.checkCancellation()
                let span = state.spans[index]
                guard span.last > span.first else { continue }
                let reset: Bool
                if span.last - span.first < 8 {
                    reset = try repairShort(beats: &result.beats, span: span, snapshot: state, sampleRate: sampleRate)
                } else {
                    reset = try NativeBeatFocusDynamicRepair.apply(beats: &result.beats,
                        first: span.first, last: span.last, ratios: state.ratios,
                        symmetricRatios: state.symmetricRatios, period: state.period,
                        sampleRate: sampleRate, sourceFrameCount: sourceFrameCount, subranges: ranges[index])
                }
                if reset {
                    result.anchorSample = result.beats[0].sampleIndex
                    result.anchorBar = result.beats[0].beatInBar
                }
            }
        }
        return result
    }

    static func snapshot(beats: [NativeBeatInfo], sampleRate: Int, modelBPM100: Int) throws -> Snapshot {
        guard beats.count >= 2, sampleRate > 0, modelBPM100 > 0 else {
            throw NativeBeatFocus.Failure.invalidInput
        }
        let count = beats.count
        // The vector prefix and scalar tail have distinct multiplication order.
        let vectorCount = count < 3 ? 0 : count - (count % 2 == 0 ? 2 : 1)
        let times = beats.enumerated().map { index, beat in
            let value = index < vectorCount
                ? Double(beat.sampleIndex) * (1000 / Double(sampleRate))
                : Double(beat.sampleIndex) * 1000 * (1 / Double(sampleRate))
            return value.rounded(.toNearestOrAwayFromZero) * 0.001
        }
        var intervals = [Double](repeating: 0, count: count)
        for index in 0..<(count - 1) {
            intervals[index] = 0.5.addingProduct(times[index + 1] - times[index], 1000).rounded(.towardZero)
        }
        let mean = NativeBeatFocus.stableRegion(intervals).mean
        let measured = 60_000 / mean
        let accepts = Double(modelBPM100) * 0.008 <= measured && measured <= Double(modelBPM100) * 0.0125
        let period = accepts ? mean : (60 / Double(modelBPM100)) * 100_000
        guard period.isFinite, period > 0 else { throw NativeBeatFocus.Failure.invalidReconciliation }
        let bpm = accepts ? Float(measured) : Float(modelBPM100) * 0.01
        var ratios = intervals.map { $0 * (1 / period) }
        var symmetric = ratios.map { value -> Double in
            abs(value) <= Double.ulpOfOne ? 0 : (value < 1 ? 1 / value : value)
        }
        ratios[count - 1] = ratios[count - 2]
        symmetric[count - 1] = symmetric[count - 2]
        var spans: [Span] = [], index = 0
        while index < count - 1, spans.count < 128 {
            if symmetric[index] > 1.18, symmetric[index + 1] > 1.18 {
                let first = index
                index += 2
                while index < count, symmetric[index] > 1.18 { index += 1 }
                spans.append(.init(first: first, last: index - 1))
            }
            index += 1
        }
        return .init(times: times, intervals: intervals, ratios: ratios, symmetricRatios: symmetric,
                     period: period, bpm: bpm, spans: spans)
    }

    static func anchorIndex(count: Int, spans: [Span]) -> Int {
        guard let first = spans.first, let last = spans.last else { return (count - 1) / 2 }
        var length = max(0, first.first), begin = 0
        var end = first.first > 0 ? first.first - 1 : count - 1
        if spans.count > 1 {
            for index in 1..<spans.count {
                let gap = spans[index].first - spans[index - 1].last
                if length < gap {
                    begin = spans[index - 1].last + 1
                    end = spans[index].first - 1
                }
                length = max(length, gap)
            }
        }
        if last.last < count - 1, length < count - 1 - last.last {
            begin = last.last + 1
            end = count - 1
        }
        // The original uses the width's midpoint, without adding its origin.
        return (end - begin) / 2
    }

    static func subranges(ratios: [Double], span: Span) -> [Span] {
        let bounds: [(Double, Double)] = [(0.4425, 0.5575), (0.59, 0.7433333333333333),
            (1.3275000000000001, 1.6724999999999999), (1.77, 2.23)]
        var result = [Span](repeating: .init(first: -1, last: -1), count: 4)
        guard span.first <= span.last else { return result }
        for index in span.first...span.last {
            for family in bounds.indices {
                let (lower, upper) = bounds[family]
                if ratios[index] > lower, ratios[index] < upper {
                    if result[family].first == -1 { result[family].first = index }
                    if index == span.last, result[family].last == -1 { result[family].last = index }
                    break
                }
                if result[family].first >= 0, result[family].last == -1, index > 0,
                   ratios[index - 1] > lower, ratios[index - 1] < upper {
                    result[family].last = index - 1
                    break
                }
            }
        }
        return result
    }

    static func repairShort(beats: inout [NativeBeatInfo], span: Span,
                            snapshot: Snapshot, sampleRate: Int) throws -> Bool {
        let values = snapshot.symmetricRatios, count = values.count
        var sum = 0.0
        if span.last - span.first == 7 {
            var even = values[span.first + 2] + values[span.first]
            var odd = values[span.first + 3] + values[span.first + 1]
            even = values[span.first + 4] + even; odd = values[span.first + 5] + odd
            even = values[span.first + 6] + even; odd = values[span.first + 7] + odd
            sum = even + odd
        } else {
            for index in span.first...span.last { sum = values[index] + sum }
        }
        let before = min(8, span.first), after = min(8, count - span.last - 1)
        var surrounding = 0.0
        if before > 0 {
            for step in 1...before { surrounding = values[span.first - step] + surrounding }
        }
        if after > 0 {
            for step in 1...after { surrounding = values[span.last + step] + surrounding }
        }
        if before + after > 0 { surrounding /= Double(before + after) }
        guard sum / Double(span.last - span.first + 1) - surrounding > 0.16 else { return false }
        var first = span.first, last = min(span.last, count - 2)
        for step in 1...3 {
            let index = span.first - step
            guard index >= 0, values[index] > 1.07 else { break }
            first = index
        }
        if span.last < count - 2 {
            for step in 1...3 {
                let index = span.last + step
                guard index <= count - 2, values[index] > 1.07 else { break }
                last = index
            }
        }
        let origin = snapshot.times[first]
        let duration = snapshot.times[last + 1] - origin
        let rawSteps = duration * 1000 * (1 / snapshot.period)
        let roundedSteps = (rawSteps + 0.5).rounded(.towardZero)
        guard roundedSteps >= 2, abs(rawSteps - roundedSteps) < 0.25 else { return false }
        guard first >= 0, last < beats.count else { throw NativeBeatFocus.Failure.invalidReconciliation }
        let insertions = (1..<Int(roundedSteps)).map { index -> NativeBeatInfo in
            let time = origin.addingProduct(duration * Double(index), 1 / roundedSteps)
            let position = Int(0.5.addingProduct(time, Double(sampleRate)))
            return .init(sampleIndex: position, intervalInBeats: 0, periodMilliseconds: 0,
                         beatInBar: 0, sourceBand: -1, flag: 0)
        }
        beats.replaceSubrange((first + 1)..<(last + 1), with: insertions)
        return true
    }
}
