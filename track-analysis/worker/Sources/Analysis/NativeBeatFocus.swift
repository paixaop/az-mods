#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Reconciles the model's four-beat event grid with the independently measured
/// tempo before the streaming audio adjustment and final bar-phase selection.
enum NativeBeatFocus {
    struct Result: Sendable {
        var beats: [NativeBeatInfo]
        var bpm: Float
        var changedTempoOctave: Bool
    }
    enum Failure: Error { case invalidInput, unsupportedMeter, invalidReconciliation }
    private struct Span { let first: Int; let last: Int; var inserted = 0 }

    static func normal(model: [NativeBeatExport.Beat], legacyBPM100: Int,
                       sampleRate: Int, sourceFrameCount: Int, windowOffset: Int,
                       lowIntegral: (Float, Int) -> Double) throws -> Result {
        guard sampleRate > 0, legacyBPM100 > 0, model.count >= 4,
              sourceFrameCount > 0, windowOffset >= 0 else { throw Failure.invalidInput }
        guard model.prefix(4).map(\.numBar).max() == 4 else { throw Failure.unsupportedMeter }
        let count = model.count
        let times = model.map { (Double($0.pos) * 1000 * (1 / Double(sampleRate))).rounded(.toNearestOrAwayFromZero) * 0.001 }
        var intervals = [Double](repeating: 0, count: count)
        for i in 0..<(count - 1) {
            intervals[i] = 0.5.addingProduct(times[i + 1] - times[i], 1000).rounded(.towardZero)
        }
        let stable = stableRegion(intervals)
        let meanInterval = stable.mean
        guard meanInterval > 0 else { throw Failure.invalidReconciliation }
        var bpm = Float(legacyBPM100) * 0.01
        let integerTempo = bpm.rounded(.toNearestOrAwayFromZero)
        if Double(integerTempo) * 0.9995 <= Double(bpm), Double(bpm) <= Double(integerTempo) * 1.0005 { bpm = integerTempo }
        let detectedTempo = 60_000 / meanInterval
        var changedTempoOctave = false
        if Double(bpm) * 1.98 <= detectedTempo, detectedTempo <= Double(bpm) * 2.02 {
            if bpm <= 90 { bpm += bpm }
        } else if Double(bpm) * 0.495 <= detectedTempo, detectedTempo <= Double(bpm) * 0.505 {
            if bpm >= 140 { bpm *= 0.5 }
        } else if detectedTempo < Double(bpm) * 0.8 || Double(bpm) * 1.25 < detectedTempo {
            bpm = Float(detectedTempo)
            if detectedTempo < 70 { bpm += bpm }
            else if detectedTempo > 180 { bpm *= 0.5; changedTempoOctave = true }
        }
        let period = (Double(sampleRate) * 60) / Double(bpm)
        var deviation = intervals.map { abs($0 - meanInterval) }
        deviation[count - 1] = deviation[count - 2]
        let narrow = spans(deviation, threshold: 10)
        let totalNarrow = narrow.reduce(0) { $0 + $1.last - $1.first + 1 }
        var wide = totalNarrow * 2 >= count ? [] : spans(deviation, threshold: 20)
        var selected = totalNarrow * 2 >= count ? narrow : wide
        let repaired = repair(times, spans: &selected, period: meanInterval)
        if totalNarrow * 2 < count { wide = selected }
        guard repaired.count >= 2 else { throw Failure.invalidReconciliation }
        let n = repaired.count
        let originalPositions = (0..<n).map { model[min($0, count - 1)].pos }
        let originalBars = (0..<n).map { model[min($0, count - 1)].numBar }
        let discordant = phaseDisagreement(narrow, positions: originalPositions, period: period)
            || phaseDisagreement(wide, positions: originalPositions, period: period)
        var anchor = 0, first = 0, last = 0
        let sourceAnchor: Int
        if discordant {
            var energy = [Double](repeating: 0, count: n)
            for i in 0..<(n - 1) {
                energy[i] = lowIntegral(bpm, windowOffset + originalPositions[i])
                if i > 0, energy[i] < 0 { energy[i] = energy[i - 1] }
            }
            let peak = max(0, energy.dropLast().max() ?? 0)
            if abs(peak) > Double.ulpOfOne {
                for i in 0..<(n - 1) { energy[i] *= 1 / peak }
            }
            for i in 0..<(n - 1) {
                if energy[i] > 0.6 || (i > 0 && energy[i] > 0.2 && abs(energy[i] - energy[i - 1]) > 0.1) { anchor = i; break }
            }
            sourceAnchor = anchor
            var bounds = between(wide, containing: anchor) ?? (0, 0)
            bounds = between(narrow, containing: anchor) ?? bounds
            let firstWide = wide.first?.first ?? 0
            if firstWide >= anchor { first = 0; last = firstWide }
            else {
                (first, last) = bounds
                if let end = wide.last?.last, end <= anchor { first = end; last = n - 1 }
            }
            let firstNarrow = narrow.first?.first ?? 0
            if firstNarrow >= anchor { first = 0; last = firstNarrow }
            else if let end = narrow.last?.last, end <= anchor { first = end; last = n - 1 }
            if first == 0, last == 0 { first = anchor; last = min(anchor + 16, n - 1) }
        } else {
            sourceAnchor = stable.first
        }
        guard sourceAnchor < originalPositions.count else { throw Failure.invalidReconciliation }
        var positions = repaired.enumerated().map { i, value -> Int in
            let ms = (value * 1000).rounded(.toNearestOrAwayFromZero)
            return i < n - n % 2 ? Int(ms * (Double(sampleRate) * 0.001)) : Int(ms * 0.001 * Double(sampleRate))
        }
        // Repair rewrites the active prefix of the original event storage. The
        // stable interval can still include its retained tail when repair removes
        // an event, so keep those source positions for the timing average.
        let statisticsPositions = positions + model.dropFirst(n).map(\.pos)
        let nearest = nearestIndex(positions, to: originalPositions[sourceAnchor])
        var barOrigin = originalBars[sourceAnchor] + ((~nearest) | -4)
        if barOrigin < 0 { barOrigin += 4 }
        let bars = positions.indices.map { (barOrigin + $0) % 4 + 1 }
        var halfShift = 0.0
        if !discordant {
            var delta = 0
            for span in wide where span.last <= stable.first { delta += span.inserted - (span.last - span.first + 1) }
            first = stable.first + delta; last = stable.last + delta
            anchor = (first + last) / 2
            guard first >= 0, last < statisticsPositions.count, anchor >= 0, anchor < n else { throw Failure.invalidReconciliation }
            if bars[anchor] != 1 { anchor = anchor - bars[anchor] + 1 }
            guard anchor >= 0, anchor < n else { throw Failure.invalidReconciliation }
            var signed = 0.0, absolute = 0.0
            for i in 0..<first {
                let error = timingError(positions, index: i, first: max(0, i - 5), last: min(first - 1, i + 5), anchor: anchor, period: period)
                signed += error; absolute += abs(error)
            }
            if last + 1 < n {
                for i in (last + 1)..<n {
                    let error = timingError(positions, index: i, first: max(last + 1, i - 5), last: min(n - 1, i + 5), anchor: anchor, period: period)
                    signed += error; absolute += abs(error)
                }
            }
            let outside = first + n - last - 1
            if outside > 0, absolute / Double(outside) > period * 0.3, Double(outside) / Double(n) > 0.5 {
                halfShift = period * (signed > 0 ? 0.5 : -0.5)
            }
        }
        guard first >= 0, last < statisticsPositions.count, anchor >= 0, anchor < n else { throw Failure.invalidReconciliation }
        let errors: [Double] = first <= last ? (first...last).map {
            Double(statisticsPositions[$0]) - Double(positions[anchor]).addingProduct(Double($0 - anchor), period)
        } : []
        let correction = errors.isEmpty ? 0 : nativeSum(errors) / Double(errors.count)
        let origin = correction + halfShift + Double(positions[anchor])
        let ideal = positions.indices.map { origin.addingProduct(Double($0 - anchor), period) }
        for i in positions.indices { positions[i] = Int(ideal[i] + 0.5) + windowOffset }
        var result = positions.indices.map { i -> NativeBeatInfo in
            let next = min(i + 1, n - 1), prior = i == n - 1 ? n - 2 : i
            let interval = Float(((ideal[next] - ideal[prior]) * 1000) / Double(sampleRate))
            return .init(sampleIndex: positions[i], intervalInBeats: 0, periodMilliseconds: interval,
                         beatInBar: bars[i], sourceBand: -1, flag: 0)
        }
        let head = result[0], tail = result[n - 1]
        let preceding = max(0, Int(Double(head.sampleIndex) / period))
        if preceding > 0 {
            for i in 1...preceding {
                let position = max(0, Int(0.5.addingProduct(-Double(i), period) + Double(head.sampleIndex)))
                var bar = (head.beatInBar - i - 1) % 4
                if bar < 0 { bar += 4 }
                result.insert(.init(sampleIndex: position, intervalInBeats: 0, periodMilliseconds: 60_000 / bpm,
                                    beatInBar: bar + 1, sourceBand: -1, flag: 0), at: 0)
            }
        }
        let following = max(0, Int(Double(sourceFrameCount - tail.sampleIndex) / period))
        if following > 0 {
            for i in 1...following {
                let position = min(sourceFrameCount, Int(0.5.addingProduct(Double(i), period) + Double(tail.sampleIndex)))
                result.append(.init(sampleIndex: position, intervalInBeats: 0, periodMilliseconds: 60_000 / bpm,
                                    beatInBar: (tail.beatInBar + i - 1) % 4 + 1, sourceBand: -1, flag: 0))
            }
        }
        return .init(beats: result, bpm: bpm, changedTempoOctave: changedTempoOctave)
    }

    static func stableRegion(_ intervals: [Double]) -> (first: Int, last: Int, mean: Double) {
        let n = intervals.count, needed = min(n, 37), limit = n > 21 ? n - 21 : (n > 1 ? 1 : n - 1)
        for threshold in [11.0, 20.0, 30.0] {
            var first = 0, length = 0, sum = 0.0
            if limit > 0 {
                for i in 0..<limit {
                    if abs(intervals[i + 1] - intervals[i]) <= threshold {
                        if length == 0 { first = i }
                        length += 1; sum += intervals[i]
                    } else {
                        if length >= needed { return (first, i - 1, sum / Double(length)) }
                        length = 0; sum = 0
                    }
                }
            }
        }
        var even = 0.0, odd = 0.0
        for i in 0..<(needed / 2) { even += intervals[i * 2]; odd += intervals[i * 2 + 1] }
        var sum = even + odd
        if needed % 2 != 0 { sum += intervals[needed - 1] }
        return (0, max(0, needed - 1), sum / Double(needed))
    }

    private static func spans(_ deviation: [Double], threshold: Double) -> [Span] {
        var result: [Span] = [], i = 0
        let end = deviation.count - 2
        while i < end, result.count < 128 {
            if deviation[i] > threshold, deviation[i + 1] > threshold {
                let first = i + 2
                i += 1
                while i < end { i += 1; if deviation[i] <= threshold { break } }
                i += 1
                result.append(.init(first: first, last: i))
            } else { i += 1 }
        }
        return result
    }

    private static func repair(_ times: [Double], spans: inout [Span], period: Double) -> [Double] {
        guard !spans.isEmpty else { return times }
        let milliseconds = times.map { $0 * 1000 }, n = times.count
        var result = [times[0]]
        for k in spans.indices {
            let first = spans[k].first, last = spans[k].last
            let start = first == 1 ? 0 : first - 2
            let end = min(n - 1, last)
            let duration = milliseconds[end] - milliseconds[start]
            let steps = Int((duration * (1 / period)).rounded(.toNearestOrAwayFromZero))
            let added = last < n ? steps - 1 : steps
            let interval = last < n && steps != 0 ? duration / Double(steps) : period
            spans[k].inserted = added
            let copyStart = k == 0 ? 1 : spans[k - 1].last + 1
            if copyStart < first - 1 { result.append(contentsOf: times[copyStart..<(first - 1)]) }
            if added > 0, first > 1 {
                for j in 1...added { result.append(milliseconds[start].addingProduct(Double(j), interval).rounded(.toNearestOrAwayFromZero) * 0.001) }
            }
            if added > 0, last < n - 1 { result.append(times[last]) }
        }
        if let last = spans.last?.last, last + 1 < n { result.append(contentsOf: times[(last + 1)...]) }
        return result
    }

    private static func phaseDisagreement(_ spans: [Span], positions: [Int], period: Double) -> Bool {
        guard spans.count > 1 else { return false }
        let indices = spans.indices.map { $0 == 0 ? spans[0].first / 2 : (spans[$0].first + spans[$0 - 1].last) / 2 }
        var positive = 0, total = 0
        for i in indices.indices {
            for j in (i + 1)..<indices.count where indices[i] < positions.count && indices[j] < positions.count {
                let value = abs(Double(positions[indices[i]] - positions[indices[j]]) * (1 / period))
                let fraction = value - value.rounded(.towardZero)
                if fraction > 0.4, fraction < 0.6 { positive += 1 }
                total += 1
            }
        }
        return Double(positive) > Double(total) * 0.25
    }

    private static func between(_ spans: [Span], containing index: Int) -> (Int, Int)? {
        guard spans.count > 1 else { return nil }
        for i in 0..<(spans.count - 1) where spans[i].last <= index && index <= spans[i + 1].first {
            return (spans[i].last, spans[i + 1].first)
        }
        return nil
    }

    private static func nearestIndex(_ positions: [Int], to target: Int) -> Int {
        var index = 0
        while index + 1 < positions.count, abs(positions[index + 1] - target) < abs(positions[index] - target) { index += 1 }
        return index
    }

    private static func timingError(_ positions: [Int], index: Int, first: Int, last: Int, anchor: Int, period: Double) -> Double {
        let displacement = period * Double(anchor - index)
        let offset = displacement - Double(positions[anchor])
        var best = offset + Double(positions[first])
        if first <= last {
            for candidate in first...last {
                let error = offset + Double(positions[candidate])
                if abs(error) < abs(best) { best = error }
            }
        }
        let own = (displacement + Double(positions[index])) - Double(positions[anchor])
        return abs(best) * 1.25 <= abs(own) ? best : own
    }

    private static func nativeSum(_ values: [Double]) -> Double {
        let vectorCount = values.count > 8 ? values.count - (values.count % 8 == 0 ? 8 : values.count % 8) : 0
        var lanes = [Double](repeating: 0, count: 8)
        if vectorCount > 0 {
            for i in 0..<vectorCount { lanes[i % 8] += values[i] }
        }
        var sum = lanes[6] + lanes[4] + lanes[2] + lanes[0] + lanes[7] + lanes[5] + lanes[3] + lanes[1]
        for i in vectorCount..<values.count { sum += values[i] }
        return sum
    }
}
