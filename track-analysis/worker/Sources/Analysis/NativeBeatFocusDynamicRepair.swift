#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Repairs sustained metrical-rate changes in the dynamic focus pass. The
/// detection arrays belong to the beginning of the pass; edits run backwards.
enum NativeBeatFocusDynamicRepair {
    private struct Statistics {
        let mean: Double
        let variance: Double
        let slope: Double
        let valid: Bool
    }
    private enum Rate: Int { case half, twoThirds, threeHalves, double }

    static func apply(beats: inout [NativeBeatInfo], first: Int, last: Int,
                      ratios: [Double], symmetricRatios: [Double],
                      period: Double, sampleRate: Int, sourceFrameCount: Int,
                      subranges: [NativeBeatFocusDynamic.Span]) throws -> Bool {
        guard first >= 0, last >= first, last < ratios.count,
              ratios.count == symmetricRatios.count, period > 0, sampleRate > 0,
              subranges.count == 4 else { throw NativeBeatFocus.Failure.invalidInput }
        let whole = statistics(ratios, first: first, last: last, vector: true)
        let bounds: [(Double, Double)] = [(0.485, 0.515),
            (0.6466666666666666, 0.6866666666666666), (1.455, 1.545), (1.94, 2.06)]
        var selection: (Rate, Int, Int)?
        for raw in 0..<4 {
            let range = subranges[raw]
            if whole.valid, whole.mean > bounds[raw].0, whole.mean < bounds[raw].1 {
                selection = (Rate(rawValue: raw)!, first, last)
                break
            }
            let part = statistics(ratios, first: range.first, last: range.last,
                                  vector: false, restrictTo128: raw == 0)
            if part.valid {
                selection = (Rate(rawValue: raw)!, range.first, range.last)
                break
            }
        }
        guard let (rate, selectedFirst, selectedLast) = selection else { return false }
        func unstable(_ value: Double) -> Bool {
            switch rate {
            case .half, .double: return (value > 1.07 && value < 1.8) || value > 2.2
            case .twoThirds, .threeHalves:
                return (value > 1.07 && value < 1.4249999999999998)
                    || (value > 1.5750000000000002 && value < 1.8)
            }
        }
        var reachedLimit = false
        func boundary(_ origin: Int, direction: Int) -> Int {
            var result = origin
            for step in 1...5 {
                let next = origin + step * direction
                guard next >= 0, next < symmetricRatios.count,
                      unstable(symmetricRatios[next]) else { break }
                result = next
                if step == 5 { reachedLimit = true }
            }
            return result
        }
        let leftStart = boundary(selectedFirst, direction: -1)
        let leftEnd = boundary(selectedFirst, direction: 1)
        let rightStart = boundary(selectedLast, direction: -1)
        let rightEnd = boundary(selectedLast, direction: 1)
        guard !beats.isEmpty else { return false }
        let rightHigh = min(rightEnd, beats.count - 1)
        var end = rightHigh
        let removedRight = rightStart < rightHigh
        if removedRight {
            beats.removeSubrange((rightStart + 1)...rightHigh)
            end = rightStart
        }
        let start = max(0, leftStart)
        let leftHigh = min(leftEnd, beats.count - 1)
        let removedLeft = start < leftHigh
        if removedLeft {
            beats.removeSubrange((start + 1)...leftHigh)
            end -= leftHigh - start
        }
        guard start >= 0, start < beats.count, end >= 0, end < beats.count else {
            throw NativeBeatFocus.Failure.invalidReconciliation
        }

        var alignedLeft = false, alignedRight = false
        if rate != .double {
            if let match = alignedBeat(beats, origin: start, direction: 1,
                                       period: period, sampleRate: sampleRate), match > start + 1 {
                beats.removeSubrange((start + 1)..<match)
                end -= match - start - 1
                alignedLeft = true
            }
            if let match = alignedBeat(beats, origin: end, direction: -1,
                                       period: period, sampleRate: sampleRate), match < end - 1 {
                // The original loop keeps its erase pointer at end-1 while
                // decrementing the logical endpoint. Preserve that order: a
                // contiguous erase beginning at the old penultimate event.
                let count = end - 1 - match
                guard end - 1 + count <= beats.count else {
                    throw NativeBeatFocus.Failure.invalidReconciliation
                }
                beats.removeSubrange((end - 1)..<(end - 1 + count))
                end = match + 1
                alignedRight = true
            }
            // Keep the first interior event, then drop one (or two) events for
            // each retained event. This is deliberately not phase reindexing.
            var index = start + 1
            while index < end {
                index += 1
                let removals = rate == .twoThirds ? 2 : 1
                for _ in 0..<removals where index < beats.count {
                    beats.remove(at: index)
                    end -= 1
                }
            }
        }
        let replaceRight = removedRight || alignedRight
        let replaceLeft = removedLeft || alignedLeft
        if rate != .half {
            let upper = min(end - ((replaceRight || end == beats.count - 1) ? 1 : 0), beats.count - 2)
            let lower = start + (replaceLeft ? 1 : 0)
            if lower <= upper {
                for index in stride(from: upper, through: lower, by: -1) {
                    let a = beats[index].sampleIndex, b = beats[index + 1].sampleIndex
                    if rate == .threeHalves {
                        let third = (Double(b - a) * 0.3333333333333333) / Double(sampleRate)
                        beats.insert(blank(Int(Double(a) + 0.5 + (third + third) * Double(sampleRate))), at: index + 1)
                        beats.insert(blank(Int(Double(a) + 0.5 + Double(sampleRate) * third)), at: index + 1)
                        end += 2
                    } else {
                        beats.insert(blank(Int(Double(a) + 0.5 + Double(b - a) * 0.5)), at: index + 1)
                        end += 1
                    }
                }
            }
        }
        if replaceRight, end + 1 < beats.count {
            end += fillGap(&beats, after: end, period: period, sampleRate: sampleRate)
        }
        if replaceLeft, start + 1 < beats.count {
            end += fillGap(&beats, after: start, period: period, sampleRate: sampleRate)
        }
        if beats.count >= 2 {
            let first = beats[0].sampleIndex, interval = beats[1].sampleIndex - first
            let preceding = first - interval
            if preceding != 0, interval <= first { beats.insert(blank(preceding), at: 0) }
            let last = beats[beats.count - 1].sampleIndex
            let tailInterval = last - beats[beats.count - 2].sampleIndex
            if tailInterval != 0 {
                let remaining = Double(sourceFrameCount - last) / Double(tailInterval)
                if remaining > 1 {
                    for index in 1...Int(remaining) { beats.append(blank(last + index * tailInterval)) }
                }
            }
        }
        if reachedLimit, beats.count > 3 {
            for index in 1..<(beats.count - 2) {
                let before = beats[index].sampleIndex - beats[index - 1].sampleIndex
                let gap = beats[index + 1].sampleIndex - beats[index].sampleIndex
                let after = beats[index + 2].sampleIndex - beats[index + 1].sampleIndex
                if gap > before * 5 || gap > after * 5 {
                    if after != 0 {
                        let ratio = Double(gap) / Double(after)
                        if ratio > 1, Int(ratio) > 1 {
                            let divisions = Int(ratio), step = Int(Double(gap) / Double(divisions))
                            let position = beats[index].sampleIndex
                            for offset in 1..<divisions {
                                beats.insert(blank(position + offset * step), at: index + offset)
                            }
                        }
                    }
                    break
                }
            }
        }
        return false
    }

    private static func statistics(_ values: [Double], first: Int, last: Int,
                                   vector: Bool, restrictTo128: Bool = false) -> Statistics {
        let length = last - first, count = length + 1
        guard first >= 0, last >= first, last < values.count else {
            return .init(mean: 0, variance: 0, slope: 0, valid: false)
        }
        func sum(_ transform: (Double) -> Double) -> Double {
            if vector {
                var lanes = [Double](repeating: 0, count: 8)
                let end = count & ~7
                if end > 0 {
                    for base in stride(from: 0, to: end, by: 8) {
                        for lane in 0..<8 { lanes[lane] += transform(values[first + base + lane]) }
                    }
                }
                var result = lanes[6] + lanes[4] + lanes[2] + lanes[0]
                    + lanes[7] + lanes[5] + lanes[3] + lanes[1]
                for index in end..<count { result += transform(values[first + index]) }
                return result
            }
            var result = 0.0
            for index in first...last where !restrictTo128 || (index >= 1 && index <= 127) {
                result += transform(values[index])
            }
            return result
        }
        let mean = sum { $0 } / Double(count)
        let variance = sum { ($0 - mean) * ($0 - mean) } / Double(count)
        var middle = 0.0, left = 0.0, right = 0.0
        if length > 2, first + 1 < values.count, last > 0 {
            middle = abs(values[last - 1] - values[first + 1]) / Double(length - 2)
        }
        if length > 3, first + 2 < values.count, last > 1 {
            left = abs(values[last - 2] - values[first + 1]) / Double(length - 3)
            right = abs(values[last - 1] - values[first + 2]) / Double(length - 3)
        }
        let slope = (right + middle + left) * 0.3333333333333333
        return .init(mean: mean, variance: variance, slope: slope,
                     valid: length >= 8 && slope < 0.008 && variance < 0.015)
    }

    private static func alignedBeat(_ beats: [NativeBeatInfo], origin: Int, direction: Int,
                                    period: Double, sampleRate: Int) -> Int? {
        guard beats.indices.contains(origin) else { return nil }
        let base = Double(beats[origin].sampleIndex) / Double(sampleRate)
        let tolerance = period * 0.00025
        let count = min(5, direction > 0 ? beats.count - origin - 1 : origin)
        guard count > 0 else { return nil }
        for multiple in 1...3 {
            let target = base + Double(direction) * (period * (Double(multiple) * 0.001))
            for offset in 1...count {
                let index = origin + offset * direction
                let position: Double = direction < 0 && multiple == 1
                    ? Double(beats[index].sampleIndex) / Double(sampleRate)
                    : Double(beats[index].sampleIndex) * (1 / Double(sampleRate))
                if position - tolerance < target, target < position + tolerance { return index }
            }
        }
        return nil
    }

    private static func fillGap(_ beats: inout [NativeBeatInfo], after index: Int,
                                period: Double, sampleRate: Int) -> Int {
        let start = Double(beats[index].sampleIndex) / Double(sampleRate)
        let gap = Double(beats[index + 1].sampleIndex) / Double(sampleRate) - start
        let count = Int((1 / period) * gap * 1000 + 0.5)
        guard count > 1 else { return 0 }
        for offset in 1..<count {
            let position = Int(Double(sampleRate) * (start + (1 / Double(count)) * gap * Double(offset)) + 0.5)
            beats.insert(blank(position), at: index + offset)
        }
        return count - 1
    }

    private static func blank(_ sampleIndex: Int) -> NativeBeatInfo {
        .init(sampleIndex: sampleIndex, intervalInBeats: 0, periodMilliseconds: 0,
              beatInBar: 0, sourceBand: -1, flag: 0)
    }
}
