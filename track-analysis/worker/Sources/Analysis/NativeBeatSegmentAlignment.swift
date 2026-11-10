#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativeBeatGridRepair {
    /// The segment records reuse beatInBar for the selected stride and
    /// sourceBand for the number of leading beats before the chosen anchor.
    static func alignSegments(_ segments: inout [NativeBeatInfo], candidates: [NativeBeatInfo],
                              sampleRate: Int, sourceFrameCount: Int) throws {
        guard !candidates.isEmpty else { return }
        let positions = candidates.map(\.sampleIndex)
        func preceding(_ sample: Int) -> Int {
            var low = 0, high = positions.count - 1
            while low <= high {
                let middle = low + (high - low) / 2
                if positions[middle] <= sample { low = middle + 1 }
                else { high = middle - 1 }
            }
            return high
        }
        func boundary(_ sample: Int, margin: Int, lower: Int, upper: Int) -> Double {
            let index = preceding(sample)
            let value = index < 1 ? positions[0] - margin : positions[index - 1] + margin
            return Double(max(0, min(upper, max(lower, value))))
        }
        func nearest(_ sample: Int, margin: Int) -> Int? {
            let first = max(0, preceding(sample)), last = max(0, preceding(sample + margin))
            guard first <= last else { return nil }
            var selected: Int?, distance = Int.max
            for index in first...last {
                let delta = abs(sample - positions[index])
                if delta <= margin {
                    if delta < distance { selected = index; distance = delta }
                    if positions[index] > sample { break }
                }
            }
            return selected
        }
        for index in segments.indices {
            try Task.checkCancellation()
            var score = 0.0, selectedOffset = -1.0, selectedPeriod = 0.0
            var selectedStride = 0.0, selectedLeading = 0.0
            var accepted = false
            for adjustment in [0.0, 1.0, -1.0, 2.0, -2.0] {
                let milliseconds = Double(segments[index].periodMilliseconds) + adjustment
                let period = (Double(sampleRate) * 0.001) * milliseconds
                guard period > 0 else { continue }
                let eighth = Int(period * 0.125)
                let start = index == 0 ? 0 : boundary(segments[index].sampleIndex, margin: eighth,
                    lower: segments[index - 1].sampleIndex, upper: segments[index].sampleIndex)
                let end = index == segments.count - 1 ? Double(sourceFrameCount)
                    : boundary(segments[index + 1].sampleIndex, margin: eighth,
                        lower: segments[index].sampleIndex, upper: segments[index + 1].sampleIndex)
                let margin = max(sampleRate / 20, eighth)
                let threshold = (Double(margin) * ((end - start) * 0.5)) / period
                var multiplier = 4.0
                while multiplier >= 1 {
                    let step = multiplier * period
                    let stop = min(start + step, end)
                    var anchor = start, iteration = 0
                    while anchor < stop {
                        if iteration % 64 == 0 { try Task.checkCancellation() }
                        var position = anchor, firstHit = -1.0, candidateScore = 0.0
                        while position < end {
                            let target = Int(position)
                            if let found = nearest(target, margin: margin), positions[found] >= 0 {
                                candidateScore += Double(margin - abs(positions[found] - target))
                                position = Double(positions[found])
                                if firstHit < 0 { firstHit = position }
                            }
                            position += step
                        }
                        if score < candidateScore {
                            let hit = firstHit < 0 ? anchor : firstHit
                            selectedLeading = ((hit - start) * (1 / period)).rounded(.towardZero)
                            selectedOffset = hit.addingProduct(-selectedLeading, period)
                            selectedStride = multiplier * 4
                            selectedPeriod = milliseconds
                            score = candidateScore
                        }
                        anchor += 96
                        iteration += 1
                    }
                    if adjustment == 0, selectedOffset >= 0, score * multiplier > threshold {
                        accepted = true
                        break
                    }
                    multiplier *= 0.5
                    score += score
                }
                if accepted { break }
                while multiplier < 4 { multiplier += multiplier; score *= 0.5 }
            }
            if selectedOffset >= 0 {
                segments[index].sampleIndex = Int(selectedOffset)
                segments[index].periodMilliseconds = Float(selectedPeriod)
                segments[index].beatInBar = Int(selectedStride)
                segments[index].sourceBand = Int(selectedLeading)
            }
        }
    }
}
