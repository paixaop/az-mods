#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Chooses the preliminary grid using tempo changes, local jitter and timing drift.
enum NativeBeatDynamics {
    struct Selection: Equatable {
        var selectVariable: Bool
        var refinedVariableBPM: Float
        var refinedConstantBPM: Float
        var extraBool: Bool
        var changeFlag: Bool
        var jitterFlag: Bool
        var driftFlag: Bool
    }

    static func select(variableGrid: [NativeBeatInfo], sampleRate: Int,
                       constantBPM: Float, variableBPM: Float, peakDynamic: Bool) -> Selection {
        let count = variableGrid.count
        guard count >= 2, sampleRate > 0, abs(constantBPM) > Float.ulpOfOne,
              abs(variableBPM) > Float.ulpOfOne else {
            return .init(selectVariable: false, refinedVariableBPM: variableBPM,
                         refinedConstantBPM: constantBPM,
                         extraBool: false, changeFlag: false, jitterFlag: false, driftFlag: false)
        }
        let vectorEnd = count < 3 ? 0 : count - (count % 2 == 0 ? 2 : 1)
        let times = variableGrid.enumerated().map { index, beat -> Double in
            let value = index < vectorEnd
                ? Double(beat.sampleIndex) * (1000 / Double(sampleRate))
                : (Double(beat.sampleIndex) * 1000) * (1 / Double(sampleRate))
            return value.rounded(.toNearestOrAwayFromZero) * 0.001
        }
        let intervalVectorEnd = (count - 1) / 4 * 4
        var intervals = [Double](repeating: 0, count: count)
        for index in 0..<(count - 1) {
            let difference = abs(times[index + 1] - times[index])
            intervals[index] = index < intervalVectorEnd ? difference * 1000 + 0.5
                : 0.5.addingProduct(difference, 1000)
        }
        let originalIntervals = intervals
        let tail = count < 22 ? max(0, count - 2) : 20
        let limit = count - tail - 1
        let differences = (0..<limit).map { abs(intervals[$0 + 1] - intervals[$0]) }
        let changeFlag = normalizeTempoChanges(intervals: &intervals, differences: differences, times: times)

        let averageTail = count < 53 ? max(count, 16) - 17 : 36
        let averageCount = max(0, count - averageTail - 1)
        let averages = (0..<averageCount).map { index -> Double in
            let length = min(16, count - index - 1)
            guard length > 0 else { return 0 }
            return intervals[index..<(index + length)].reduce(0, +) / Double(length)
        }
        let referencePeriod = stablePeriod(intervals: intervals, differences: differences,
                                          minimumRun: min(count, 37), fallbackBPM: variableBPM)
        let deviations = averages.map { abs($0 - referencePeriod) }
        var acceptedDeviation = 0.0, acceptedCount = 0
        for (average, deviation) in zip(averages, deviations) {
            var accepted = abs(average) <= Double.ulpOfOne
            if !accepted {
                var ratio = average * (1 / referencePeriod)
                if 1 <= referencePeriod / average { ratio = referencePeriod / average }
                accepted = ratio < 1.05
            }
            if accepted { acceptedCount += 1; acceptedDeviation += deviation }
        }
        let meanDeviation: Double
        if acceptedCount > 0, Double(acceptedCount) * 100 / Double(averageCount) > 70 {
            meanDeviation = acceptedDeviation / Double(acceptedCount)
        } else {
            meanDeviation = averageCount == 0 ? 0 : sum8(deviations) / Double(averageCount)
        }
        var run = 0, stableCount = 0
        for difference in differences {
            let next = difference <= 11 ? run + 1 : 0
            if run > 23, difference > 11 { stableCount += run }
            run = next
        }
        let stableFraction = Double(stableCount) / Double(count)
        let jitterFlag = (meanDeviation >= 2.25 && stableFraction <= 0.19)
            || (meanDeviation > 1.56 && stableFraction < 0.057)

        var tempo = variableBPM, extra = false
        let integerTempo = tempo.rounded(.toNearestOrAwayFromZero)
        if Double(tempo) <= Double(integerTempo) * 1.0005,
           Double(integerTempo) * 0.9995 <= Double(tempo) { tempo = integerTempo }
        let inferred = 60000 / referencePeriod, current = Double(tempo)
        if current * 1.98 <= inferred, inferred <= current * 2.02 {
            if tempo <= 90 { tempo += tempo }
        } else if current * 0.495 <= inferred, inferred <= current * 0.505 {
            if tempo >= 140 { tempo *= 0.5 }
        } else if !(current * 0.8 <= inferred && inferred <= current * 1.25) {
            if inferred >= 70 {
                tempo = Float(inferred)
                if inferred > 180 { tempo *= 0.5; extra = true }
            } else { tempo = Float(inferred) + Float(inferred) }
        }
        var refinedVariable = variableBPM, refinedConstant = constantBPM
        let driftFlag = !(changeFlag || jitterFlag || peakDynamic)
            && hasTimingDrift(positions: variableGrid.map(\.sampleIndex), times: times,
                              intervals: originalIntervals, referencePeriod: referencePeriod,
                              sampleRate: sampleRate, tempo: tempo,
                              variableBPM: &refinedVariable, constantBPM: &refinedConstant)
        return .init(selectVariable: changeFlag || jitterFlag || driftFlag || peakDynamic,
                     refinedVariableBPM: refinedVariable, refinedConstantBPM: refinedConstant,
                     extraBool: extra, changeFlag: changeFlag,
                     jitterFlag: jitterFlag, driftFlag: driftFlag)
    }

    private static func normalizeTempoChanges(intervals: inout [Double], differences: [Double],
                                              times: [Double]) -> Bool {
        var starts: [Int] = [], ends: [Int] = [], active = false, completed = 0
        for (index, difference) in differences.enumerated() {
            if difference <= 11 {
                if active { completed += 1; if completed >= 32 { break } }
            } else {
                if !active { starts.append(index); ends.append(index) }
                ends[ends.count - 1] = index
            }
            active = difference > 11
        }
        guard completed > 0 else { return false }
        starts = Array(starts.prefix(completed)); ends = Array(ends.prefix(completed))
        let limit = differences.count
        var means: [Double] = []
        for segment in 0...completed {
            let start = segment == 0 ? 0 : ends[segment - 1] + 1
            let end = segment == completed ? limit : starts[segment]
            let mean = start < end ? sum8(Array(intervals[start..<end])) / Double(end - start)
                : intervals[start]
            means.append(mean)
            if start < end, abs(mean) > Double.ulpOfOne {
                for index in start..<end { intervals[index] *= 1 / mean }
            }
            if segment < completed {
                for index in starts[segment]...ends[segment] { intervals[index] = 1 }
            }
        }
        var lengths: [Int] = [], durations: [Float] = []
        lengths.append(starts[0] - 1)
        durations.append(Float(times[starts[0]] - times[1]))
        if completed > 1 {
            for segment in 1..<completed {
                lengths.append(starts[segment] - ends[segment - 1] - 1)
                durations.append(Float(times[starts[segment]] - times[ends[segment - 1] + 1]))
            }
        }
        lengths.append(limit - ends[completed - 1] - 1)
        durations.append(Float(times[limit] - times[ends[completed - 1] + 1]))
        var longest = 0
        for index in 1..<lengths.count where lengths[index] > lengths[longest] { longest = index }
        let reference = means[longest]
        for segment in 0...completed {
            let mean = means[segment]
            let related = (reference * 0.485 < mean && mean < reference * 0.515)
                || (reference * 0.6466666666666666 < mean && mean < reference * 0.6866666666666666)
                || (reference * 1.455 < mean && mean < reference * 1.545)
                || (reference * 1.94 < mean && mean < reference * 2.06)
            let scale = related ? reference : mean
            let start = segment == 0 ? 0 : ends[segment - 1] + 1
            let end = segment == completed ? limit : ends[segment] + 1
            if start < end { for index in start..<end { intervals[index] *= scale } }
        }
        let tempos: [Float] = means.map { abs(Float($0)) <= Float.ulpOfOne ? 120 : 60000 / Float($0) }
        let integerTempos = tempos.map { Int($0.rounded(.toNearestOrAwayFromZero)) }
        let maxFraction = zip(tempos, integerTempos).map { abs($0 - Float($1)) }.max() ?? 0
        let weights: [Float] = durations.map { abs(times[limit]) > Double.ulpOfOne
            ? Float(Double($0) * (1 / times[limit])) : 0.5 }
        let maximum = weights.max() ?? 0
        var minimum: Float = 1
        for index in weights.indices where lengths[index] > 16 && weights[index] < minimum { minimum = weights[index] }
        var different = false
        for a in 0..<completed where lengths[a] > 16 && weights[a] > 0.075 {
            for b in a...completed where lengths[b] > 16 && weights[b] > 0.075 {
                if integerTempos[a] != integerTempos[b] { different = true }
            }
        }
        return maxFraction < 0.2 && maximum < 0.65 && minimum > 0.075
            && maximum - minimum < 0.55 && different
    }

    private static func stablePeriod(intervals: [Double], differences: [Double],
                                     minimumRun: Int, fallbackBPM: Float) -> Double {
        for threshold in [11.0, 20.0, 30.0] {
            var length = 0, total = 0.0, bestLength = 0, bestTotal = 0.0
            for (index, difference) in differences.enumerated() {
                if difference > threshold {
                    if length >= minimumRun, length > bestLength { bestLength = length; bestTotal = total }
                    length = 0; total = 0
                } else { length += 1; total += intervals[index] }
            }
            if length >= minimumRun, length > bestLength { bestLength = length; bestTotal = total }
            if bestLength > 0 { return bestTotal / Double(bestLength) }
        }
        guard !differences.isEmpty else { return (60 / Double(fallbackBPM)) * 1000 }
        let sorted = intervals.prefix(differences.count).sorted(by: >), middle = sorted.count / 2
        return sorted.count % 2 == 1 ? sorted[middle] : (sorted[middle] + sorted[middle - 1]) * 0.5
    }

    /// Eight-lane reduction order used by the original SIMD loops.
    static func sum8(_ values: [Double]) -> Double {
        let end = values.count / 8 * 8
        var lanes = [Double](repeating: 0, count: 8)
        for index in 0..<end { lanes[index % 8] += values[index] }
        var total = lanes[6] + lanes[4] + lanes[2] + lanes[0]
        total += lanes[7]; total += lanes[5]; total += lanes[3]; total += lanes[1]
        for index in end..<values.count { total += values[index] }
        return total
    }
}
