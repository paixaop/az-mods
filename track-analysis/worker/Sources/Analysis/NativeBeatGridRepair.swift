#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Preliminary-grid refinement before envelope phase selection and the model.
enum NativeBeatGridRepair {
    static func correctSpan(_ beats: [NativeBeatInfo], sampleRate: Int, sourceFrameCount: Int,
                            first: Int, last: Int) -> [NativeBeatInfo] {
        let first = max(0, first), last = last < 0 ? beats.count - 1 : last
        guard first < last, first < beats.count, last < beats.count else { return beats }
        let a = Double(beats[first].sampleIndex), b = Double(beats[last].sampleIndex)
        let between = last - first, period = (b - a) / Double(between)
        guard period > 0 else { return beats }
        let before = Int(a / period)
        guard before >= 0 else { return beats }
        var position = a.addingProduct(-Double(before), period)
        let milliseconds = Float(period / (Double(sampleRate) * 0.001))
        let after = max(0, Int((Double(sourceFrameCount) - b) / period))
        var result: [NativeBeatInfo] = []
        for _ in 0..<(before + 1 + between + after) {
            result.append(.init(sampleIndex: Int(position), intervalInBeats: 0,
                periodMilliseconds: milliseconds, beatInBar: 0, sourceBand: -1, flag: 0))
            position += period
        }
        result[before].flag = 1
        result[before].sourceBand = beats[first].sourceBand
        result[before + between].sourceBand = beats[last].sourceBand
        return result
    }

    static func correctGaps(_ beats: inout [NativeBeatInfo]) {
        var index = 0
        while index + 3 < beats.count {
            let a = beats[index].sampleIndex, b = beats[index + 1].sampleIndex
            let c = beats[index + 2].sampleIndex, d = beats[index + 3].sampleIndex
            let left = Double(b - a), middle = Double(c - b), right = Double(d - c)
            let high = max(left, right), low = min(left, right)
            if middle / high > 1.1 {
                if high == left && !(4 <= (middle - high) / (high - low)) {
                    beats[index + 2].sampleIndex = b + Int(high)
                } else if low != left || middle.addingProduct(high, 3) <= low * 5 {
                    let extent = Double(d - a)
                    if (middle + left + right) / high < 3.3000000000000003 {
                        beats[index + 1].sampleIndex = a + Int(extent * 0.3333333333333333)
                        beats[index + 2].sampleIndex = a + Int(extent * 0.6666666666666666)
                    } else {
                        beats[index + 1].sampleIndex = a + Int(extent * 0.25)
                        beats[index + 2].sampleIndex = a + Int(extent * 0.5)
                        var added = beats[index + 2]
                        added.sampleIndex = a + Int(extent * 0.75)
                        beats.insert(added, at: index + 2)
                    }
                } else {
                    beats[index + 2].sampleIndex = b + Int(low)
                    if b + Int(low) + Int(low + low) < d {
                        var added = beats[index + 2]
                        added.sampleIndex = b + Int(low + low)
                        beats.insert(added, at: index + 2)
                    }
                }
            } else if middle / low < 0.9 {
                if low == left && (low - middle) / (high - low) < 4 {
                    beats[index + 2].sampleIndex = b + Int(low)
                } else if high == left && middle.addingProduct(low, 4) < high * 4 {
                    if b + Int(high) < d { beats[index + 2].sampleIndex = b + Int(high) }
                    else { beats.remove(at: index + 2) }
                } else if (middle + left + right) / low > 2.7 {
                    let extent = Double(d - a)
                    beats[index + 1].sampleIndex = a + Int(extent * 0.3333333333333333)
                    beats[index + 2].sampleIndex = a + Int(extent * 0.6666666666666666)
                } else {
                    beats[index + 1].sampleIndex = (d + a) / 2
                    beats.remove(at: index + 2)
                }
            }
            index += 1
        }
    }

    static func judgeDynamicPeak(candidates: [NativeBeatInfo], low: [Int16], sampleRate: Int,
                                 sourceFrameCount: Int, globalBPM: Float) -> Bool {
        var values: [Double] = []
        for (index, beat) in candidates.dropLast().enumerated() {
            if beat.sampleIndex >= sampleRate * 900 { break }
            var value = -1.0
            if abs(globalBPM) > Float.ulpOfOne, beat.sampleIndex >= -3 {
                let count = Int((Double(Float(sampleRate)) * 7.5) / Double(globalBPM) + 0.5)
                let start = Int(0.5.addingProduct(Double(beat.sampleIndex), 0.25))
                if start + count - 1 <= sourceFrameCount / 4,
                   start + count - 1 <= sampleRate * 225 {
                    if count <= 0 { value = 0 }
                    else if start >= 0, start + count <= low.count {
                        value = low[start..<(start + count)].reduce(0) { $0 + Double($1) }
                    }
                }
            }
            if index > 0, value < 0 { break }
            values.append(value)
        }
        let maximum = values.reduce(0, max)
        // These are sums of signed-16 integers, exact at the supported size.
        let mean = values.isEmpty ? 0 : values.reduce(0, +) / Double(values.count)
        return Double(candidates.count) * maximum * mean < 2e15
    }
}
