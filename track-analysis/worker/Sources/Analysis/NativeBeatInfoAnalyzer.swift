#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Consolidates overlapping onset hypotheses before grid tracking. The periods
/// remain Float values in the native 24-byte record; positions are source samples.
enum NativeBeatInfoAnalyzer {
    struct Preparation: Sendable {
        var candidates: [NativeBeatInfo]
        var segments: [NativeBeatInfo]
    }

    struct TrackedGrid: Sendable {
        var beats: [NativeBeatInfo]
        var firstReliable: Int
        var lastReliable: Int
        var firstAnchor: Int = 0
    }

    /// Tracks quarter-beat hypotheses over a 32-beat window, advancing four beats
    /// after each estimate. Band delays are milliseconds measured by the detector.
    static func track(_ candidates: [NativeBeatInfo], segments: [NativeBeatInfo],
                      sampleRate: Int, sourceFrameCount: Int) throws -> TrackedGrid {
        let delays: [Double] = [-30, -20, -3, -3, -20, -3]
        let scale = Double(sampleRate) * 0.001
        let inverseRate = 1 / Double(sampleRate)
        var result = TrackedGrid(beats: [], firstReliable: -1, lastReliable: -1)
        var finalPeriod: Double = 0
        for segmentIndex in segments.indices {
            try Task.checkCancellation()
            let segment = segments[segmentIndex]
            let end = Double(segmentIndex + 1 < segments.count ? segments[segmentIndex + 1].sampleIndex : sourceFrameCount)
            let priorPosition = Double(result.beats.last?.sampleIndex ?? 0)
            var baseCount = result.beats.count
            let skipped = segment.sourceBand
            let cadence = segment.beatInBar
            var period = scale * Double(segment.periodMilliseconds)
            var position = Double(segment.sampleIndex)
            if skipped > 0 {
                for _ in 0..<skipped {
                    position += period
                    if position >= end { break }
                }
            }
            var bands = [Int](repeating: -1, count: 129)
            let tolerance = max(sampleRate / 20, Int(period * 0.125))
            if let near = nearest(candidates, position: Int(position), tolerance: tolerance),
               candidates[near].sampleIndex >= 0 {
                position = Double(candidates[near].sampleIndex)
                bands[0] = candidates[near].sourceBand
            }
            func delay(_ index: Int) -> Double {
                delays.indices.contains(index) ? delays[index] : -3
            }
            var anchor = position + delay(bands[0]) * scale
            if segmentIndex == 0 { result.firstAnchor = Int(anchor) }
            var count = 0
            var periodMilliseconds = segment.periodMilliseconds
            let quarter = period * 0.25

            func append(_ position: Double, band: Int, flag: UInt8 = 0, reliable: Bool = true) {
                result.beats.append(.init(sampleIndex: Int(position), intervalInBeats: 0,
                    periodMilliseconds: periodMilliseconds, beatInBar: 0, sourceBand: band, flag: flag))
                if reliable && band >= 0 && band < 6 {
                    if result.firstReliable < 0 { result.firstReliable = result.beats.count - 1 }
                    else { result.lastReliable = result.beats.count - 1 }
                }
            }
            func prependAndStart() {
                if skipped >= 0 {
                    for index in -1..<skipped {
                        let earlier = anchor - Double(skipped - index) * period
                        if Int(earlier) >= Int(priorPosition) {
                            append(earlier, band: -1, reliable: false)
                            if index == -1 { baseCount += 1 }
                        } else if index >= 0 { baseCount -= 1 }
                    }
                }
                append(anchor, band: bands[0], flag: 1, reliable: false)
                if bands[0] >= 0 && bands[0] < 6 && result.firstReliable < 0 {
                    result.firstReliable = result.beats.count - 1
                }
            }
            func appendBeats(_ length: Int, startingQuarter: Int) {
                guard length > 0 else { return }
                for index in 0..<length {
                    anchor += period
                    if anchor >= end { break }
                    let slot = startingQuarter + index * 4
                    let band = cadence != 0 && slot % cadence == 0 && bands.indices.contains(slot) ? bands[slot] : -1
                    append(anchor, band: band)
                }
            }

            while position < end {
                try Task.checkCancellation()
                for _ in 0..<4 {
                    position += quarter
                    count += 1
                    bands[count] = -1
                    if cadence != 0 && count % cadence == 0,
                       let near = nearest(candidates, position: Int(position), tolerance: tolerance),
                       candidates[near].sampleIndex >= 0 {
                        position = Double(candidates[near].sampleIndex)
                        bands[count] = candidates[near].sourceBand
                    }
                }
                if count % 128 == 0 {
                    period = (position - anchor).addingProduct(scale, delay(bands[count])) * (1 / 32)
                    periodMilliseconds = Float(period * 1000 * inverseRate)
                    if result.beats.count <= baseCount + skipped { prependAndStart() }
                    if result.beats.count <= baseCount + 16 + skipped { appendBeats(16, startingQuarter: 4) }
                    if !result.beats.isEmpty {
                        result.beats[result.beats.count - 1].periodMilliseconds = periodMilliseconds
                        anchor = Double(result.beats.last!.sampleIndex)
                    }
                    appendBeats(4, startingQuarter: 68)
                    if result.beats.count > 16 { anchor = Double(result.beats[result.beats.count - 17].sampleIndex) }
                    count -= 16
                    for slot in 64...count { bands[slot] = bands[slot + 16] }
                }
            }
            if count % 128 != 0 && count >= 4 {
                let trackedCount = count
                if result.beats.count <= baseCount + skipped {
                    period = (position - anchor).addingProduct(scale, delay(bands[count])) * 4 / Double(count)
                    periodMilliseconds = Float(period * 1000 * inverseRate)
                    prependAndStart()
                }
                if result.beats.count <= baseCount + 16 + skipped {
                    period = (position - anchor).addingProduct(scale, delay(bands[count])) * 4 / Double(count)
                    periodMilliseconds = Float(period * 1000 * inverseRate)
                    appendBeats(16, startingQuarter: 4)
                }
                if let last = result.beats.last { anchor = Double(last.sampleIndex) }
                appendBeats(trackedCount / 4 - 16, startingQuarter: 68)
            }
            while anchor < end {
                anchor += period
                if anchor >= end { break }
                append(anchor, band: -1, reliable: false)
            }
            finalPeriod = period
        }
        if !result.beats.isEmpty {
            result.beats[result.beats.count - 1].periodMilliseconds = Float(finalPeriod / scale)
        }
        return result
    }

    static func compatible(_ period: Float, with reference: Float) -> Bool {
        let tolerance = Float(min(Double(reference) * 0.04, 20))
        let lower = reference - tolerance, upper = reference + tolerance
        return (lower < period && period < upper) ||
            (lower < period + period && period + period < upper) ||
            (lower < period * 0.5 && period * 0.5 < upper)
    }

    static func dominantPeriod(_ candidates: [NativeBeatInfo], start: Int = 0,
                               sampleSpan: Int, fine: Bool = true) -> Float? {
        guard candidates.indices.contains(start) else { return nil }
        let endSample = candidates[start].sampleIndex + sampleSpan
        var coarse = [Int](repeating: 0, count: 400)
        var precise = [Int](repeating: 0, count: 4000)
        for candidate in candidates[start...] {
            if candidate.sampleIndex > endSample { break }
            let period = Double(candidate.periodMilliseconds)
            guard period > 0, period.isFinite else { continue }
            let coarseBin = Int(600000 / period + 5) / 10
            let preciseBin = Int(6000000 / period + 5) / 10
            if coarse.indices.contains(coarseBin) { coarse[coarseBin] += 1 }
            if precise.indices.contains(preciseBin) { precise[preciseBin] += 1 }
        }
        func greatest(_ values: [Int], range: Range<Int>) -> Int? {
            var selected: Int?, maximum = 0
            for index in range where values[index] > maximum {
                selected = index
                maximum = values[index]
            }
            return selected
        }
        guard let coarseBin = greatest(coarse, range: coarse.indices) else { return nil }
        if !fine { return coarseBin > 0 ? 60000 / Float(coarseBin) : nil }
        guard let preciseBin = greatest(precise, range: max(0, (coarseBin - 1) * 10)..<min(4000, (coarseBin + 1) * 10)),
              preciseBin > 0 else { return nil }
        return 600000 / Float(preciseBin)
    }

    static func constantPreparation(_ input: [NativeBeatInfo], sampleRate: Int,
                                    sourceFrameCount: Int) -> Preparation {
        guard let period = dominantPeriod(input, sampleSpan: sourceFrameCount) else {
            return .init(candidates: [], segments: [])
        }
        var previous = Float(min(Double(period) * 0.04, 20))
        var candidates = [NativeBeatInfo]()
        for candidate in input {
            if compatible(candidate.periodMilliseconds, with: period) ||
                compatible(candidate.periodMilliseconds, with: previous) {
                candidates.append(candidate)
            }
            previous = candidate.periodMilliseconds
        }
        guard var first = candidates.first,
              let refined = dominantPeriod(candidates, sampleSpan: sourceFrameCount) else {
            return .init(candidates: [], segments: [])
        }
        first.periodMilliseconds = refined
        first.intervalInBeats = 0
        first.beatInBar = 4
        let segments = [first]
        removeOverlappingCandidates(&candidates, segments: segments, sampleRate: sampleRate,
                                    sourceFrameCount: sourceFrameCount)
        return .init(candidates: candidates, segments: segments)
    }

    static func variablePreparation(_ input: [NativeBeatInfo], sampleRate: Int,
                                    sourceFrameCount: Int) throws -> Preparation {
        var groupPeriods = [Float]()
        for candidate in input {
            let group = candidate.beatInBar >> 8
            guard group >= 0 else { continue }
            if group >= groupPeriods.count {
                groupPeriods += repeatElement(0, count: group + 1 - groupPeriods.count)
                groupPeriods[group] = candidate.periodMilliseconds
            } else if groupPeriods[group] == 0,
                      let next = groupPeriods[(group + 1)...].first(where: { $0 != 0 }),
                      compatible(candidate.periodMilliseconds, with: next) {
                groupPeriods[group] = candidate.periodMilliseconds
            }
        }
        let groups = groupPeriods.indices.filter { groupPeriods[$0] != 0 }
        let survivingGroups = Set(stableRuns(groups.map { groupPeriods[$0] }, minimum: 2, restorationLimit: 8)
            .map { groups[$0] })
        var candidates = input.filter { survivingGroups.contains($0.beatInBar >> 8) }
        let survivingRecords = stableRuns(candidates.map(\.periodMilliseconds), minimum: 8, restorationLimit: 32)
        candidates = survivingRecords.map { candidates[$0] }
        var segments = [NativeBeatInfo]()
        var cursor = 0
        var priorPeriod: Float = 0
        while cursor < candidates.count {
            try Task.checkCancellation()
            guard let period = dominantPeriod(candidates, start: cursor, sampleSpan: sampleRate * 8, fine: false) else {
                cursor += 1
                continue
            }
            if !compatible(period, with: priorPeriod) {
                let tempo = Int(60000 / period + 0.5)
                let exact = (cursor..<candidates.count).first {
                    Int(600000 / Double(candidates[$0].periodMilliseconds) + 5) / 10 == tempo
                }
                if let exact {
                    var first = exact
                    let tolerance = Float(min(Double(period) * 0.04, 20))
                    for factor: Float in [0.5, 1, 2] {
                        let lower = Int(60000 / ((0.5 + tolerance) + period * factor))
                        let upper = Int(60000 / ((0.5 - tolerance) + period * factor))
                        if lower < upper {
                            for index in cursor..<first {
                                let value = Int(600000 / Double(candidates[index].periodMilliseconds) + 5) / 10
                                if lower <= value && value < upper { first = index; break }
                            }
                        }
                    }
                    var segment = candidates[first]
                    segment.periodMilliseconds = candidates[exact].periodMilliseconds
                    segments.append(segment)
                    cursor = first
                }
            }
            priorPeriod = period
            cursor += 1
        }
        var segmentIndex = 0
        while segmentIndex < segments.count {
            let position = segments[segmentIndex].sampleIndex
            let span = segmentIndex + 1 < segments.count ? segments[segmentIndex + 1].sampleIndex - position : sourceFrameCount
            if let first = candidates.firstIndex(where: {
                let order = compare_timestamps(Double($0.sampleIndex), Double(position), UInt64(sampleRate), Double(sampleRate), 1)
                return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL
            }),
               let period = dominantPeriod(candidates, start: first, sampleSpan: span) {
                segments[segmentIndex].periodMilliseconds = period
                segments[segmentIndex].intervalInBeats = 0
                segments[segmentIndex].beatInBar = 4
                if segmentIndex > 0 && compatible(period, with: segments[segmentIndex - 1].periodMilliseconds) {
                    segments.remove(at: segmentIndex)
                    segmentIndex -= 1
                    continue
                }
            }
            segmentIndex += 1
        }
        removeOverlappingCandidates(&candidates, segments: segments, sampleRate: sampleRate,
                                    sourceFrameCount: sourceFrameCount)
        return .init(candidates: candidates, segments: segments)
    }

    /// Removes brief changes in tempo family and restores the preceding stable
    /// family when a short excursion returns to it.
    private static func stableRuns(_ periods: [Float], minimum: Int, restorationLimit: Int) -> [Int] {
        guard let first = periods.first else { return [] }
        var indices = Array(periods.indices)
        var start = 0, cursor = 1, count = 0, priorCount = 0
        var period = first, priorPeriod: Float = 0
        while cursor < indices.count {
            let candidate = periods[indices[cursor]]
            if compatible(candidate, with: period) {
                period = candidate
                count += 1
                cursor += 1
            } else if count < minimum || (count < restorationLimit && compatible(candidate, with: priorPeriod)) {
                indices.removeSubrange(start..<cursor)
                cursor = start
                if priorCount == 0 { period = candidate; count = 0 }
                else { period = priorPeriod; count = priorCount }
            } else {
                priorPeriod = period
                priorCount = count
                period = candidate
                start = cursor
                cursor += 1
                count = 0
            }
        }
        let terminalMinimum = minimum == 2 ? 1 : minimum
        if priorCount != 0 && count < terminalMinimum { indices.removeSubrange(start..<cursor) }
        return indices
    }

    static func removeOverlappingCandidates(_ candidates: inout [NativeBeatInfo],
                                           segments: [NativeBeatInfo], sampleRate: Int,
                                           sourceFrameCount: Int) {
        guard candidates.count > 1 else { return }
        var strengths = [Int: Int]()
        for candidate in candidates where strengths[candidate.beatInBar >> 8, default: 0] == 0 {
            strengths[candidate.beatInBar >> 8] = candidate.beatInBar & 255
        }
        var index = 1
        for segment in segments.indices {
            let endSample = segment + 1 < segments.count ? segments[segment + 1].sampleIndex : sourceFrameCount
            let tolerance = Int(Double(segments[segment].periodMilliseconds * Float(sampleRate)) * 0.000125)
            while index < candidates.count && candidates[index].sampleIndex < endSample {
                let previous = candidates[index - 1]
                let current = candidates[index]
                if abs(previous.sampleIndex - current.sampleIndex) < tolerance {
                    let previousStrength = strengths[previous.beatInBar >> 8, default: 0]
                    let currentStrength = strengths[current.beatInBar >> 8, default: 0]
                    candidates.remove(at: previousStrength < currentStrength ? index - 1 : index)
                } else { index += 1 }
            }
        }
    }

    /// The original search begins at the final candidate at or before the target.
    /// Equal-distance candidates keep the earlier candidate visited by that scan.
    static func nearest(_ candidates: [NativeBeatInfo], position: Int, tolerance: Int) -> Int? {
        guard !candidates.isEmpty else { return nil }
        func lastAtOrBefore(_ value: Int) -> Int {
            var lower = 0, upper = candidates.count
            while lower < upper {
                let middle = lower + (upper - lower) / 2
                if candidates[middle].sampleIndex <= value { lower = middle + 1 }
                else { upper = middle }
            }
            return max(0, lower - 1)
        }
        let first = lastAtOrBefore(position), last = lastAtOrBefore(position + tolerance)
        guard first <= last else { return nil }
        var best: Int?, distance = Int.max
        for index in first...last {
            let delta = abs(position - candidates[index].sampleIndex)
            if delta <= tolerance {
                if delta < distance { best = index }
                if position < candidates[index].sampleIndex { break }
                distance = min(distance, delta)
            }
        }
        return best
    }
}
