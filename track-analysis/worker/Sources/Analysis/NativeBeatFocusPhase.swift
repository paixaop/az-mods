#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Selects the four-beat bar phase after the audio position adjustment. The
/// integral callbacks use the original sample clock, including codec startup.
enum NativeBeatFocusPhase {
    enum Failure: Error { case invalidInput, insufficientBeats }

    static func postprocess(beats: inout [NativeBeatInfo], sampleRate: Int, bpm: Float,
                            lowIntegral: (Float, Int) -> Double,
                            fullIntegral: (Float, Int) -> Double) throws {
        guard sampleRate > 0, bpm.isFinite, bpm > 0 else { throw Failure.invalidInput }
        // The phase vote consumes sixteen ranked entries, including two endpoint
        // entries. Shorter grids have no defined result in the source routine.
        guard beats.count >= 14 else { throw Failure.insufficientBeats }
        let n = beats.count
        let fraction = Double(bpm) * Double(beats[0].sampleIndex) / (Double(sampleRate) * 60)
        var low = [Double](repeating: 0, count: n + 2)
        var full = low
        if fraction >= 0.5 {
            let initialBPM = Double(sampleRate) * 60 / Double(beats[0].sampleIndex)
            low[0] = lowIntegral(Float(initialBPM) * 0.5, 0) * initialBPM / Double(bpm)
            full[0] = fullIntegral(Float(initialBPM), 0) * initialBPM / Double(bpm)
        }
        for i in beats.indices {
            let value = lowIntegral(bpm * 0.5, beats[i].sampleIndex)
            low[i + 1] = i == 0 ? value : max(0, value)
        }
        for i in beats.indices {
            let value = fullIntegral(bpm, beats[i].sampleIndex)
            full[i + 1] = i == 0 ? value : max(0, value)
        }
        let lowChanges = changes(low)
        let fullChanges = changes(full)
        let irregularLow = irregularTail(lowChanges)
        let irregularFull = irregularTail(fullChanges)
        let firstIndex = fraction < 0.5 ? 1 : 0
        let window = min(n, 48)
        let beginning = statistics(fullChanges, indices: Array(firstIndex...window))
        let leadingLow = (firstIndex...n).first { lowChanges[$0] > 0.35 }
            .map { phase(at: $0, beats: beats) } ?? 0
        let leadingFull = (firstIndex...n).first {
            fullChanges[$0] > beginning.mean.addingProduct(beginning.deviation, 1.5)
        }.map { phase(at: $0, beats: beats) } ?? 0
        let endingIndices = Array(stride(from: n + 1, through: n - window + 2, by: -1))
        let lowEnding = statistics(lowChanges, indices: endingIndices)
        let fullEnding = statistics(fullChanges, indices: endingIndices)
        let endingLow = stride(from: n + 1, through: 2, by: -1).first {
            lowChanges[$0] < (-lowEnding.mean).addingProduct(lowEnding.deviation, -0.5)
        }.map { phase(at: $0, beats: beats) } ?? 0
        let endingFull = stride(from: n + 1, through: 2, by: -1).first {
            fullChanges[$0] < (-fullEnding.mean).addingProduct(fullEnding.deviation, -0.5)
        }.map { phase(at: $0, beats: beats) } ?? 0

        let onsetVotes = isolatedVotes(lowChanges, beats: beats)
        let rankVotes = rankedVotes(low, beats: beats)
        var edgeVotes = [Int](repeating: 0, count: 4)
        for value in [leadingLow, leadingFull,
                      irregularLow ? 0 : endingLow, irregularFull ? 0 : endingFull] {
            if (1...4).contains(value) { edgeVotes[value - 1] += 1 }
        }
        let firstChoice = firstMaximum(edgeVotes)
        let secondChoice = (0..<4).last { edgeVotes[$0] == edgeVotes[firstChoice] }!
        let onsetWinner = firstMaximum(onsetVotes)
        let runnerUp = (0..<4).filter { $0 != onsetWinner }.map { onsetVotes[$0] }.max()!
        let onsetChoice: Int
        if onsetVotes[onsetWinner] - runnerUp <= 10, runnerUp > 4 {
            let maximum = max(0, low.prefix(n + 1).max()!)
            if abs(maximum) > Double.ulpOfOne {
                let reciprocal = 1 / maximum
                for i in 0...n { low[i] *= reciprocal }
            }
            let votes = isolatedVotes(low, beats: beats)
            let winner = firstMaximum(votes)
            onsetChoice = votes[winner] > 0 ? winner + 1 : 0
        } else {
            onsetChoice = onsetWinner + 1
        }

        for p in 0..<4 where onsetVotes[p] > 0 {
            if (0..<4).allSatisfy({ $0 == p || onsetVotes[$0] == 0 }) {
                rotate(&beats, phase: p)
                return
            }
        }
        for p in 0..<4 where firstChoice == p || secondChoice == p {
            if onsetVotes[p] > 0, (0..<4).allSatisfy({ other in
                other == p || onsetVotes[other] < (p == 3 && other == 2 ? 11 : 2)
            }) {
                rotate(&beats, phase: p)
                return
            }
        }
        if leadingLow != 1, leadingLow == leadingFull,
           leadingLow == endingLow || leadingLow == previous(endingLow),
           leadingLow == endingFull || leadingLow == previous(endingFull) {
            offset(&beats, by: 1 - leadingLow)
            return
        }
        if firstChoice == 0, secondChoice == 0, onsetChoice == 1 { return }
        if (firstChoice != 0 && firstChoice + 1 == onsetChoice)
            || (secondChoice != 0 && secondChoice + 1 == onsetChoice) {
            offset(&beats, by: 1 - onsetChoice)
            return
        }
        for p in 0..<4 where edgeVotes[p] > 0 && rankVotes[p] > 0 {
            if (0..<4).allSatisfy({ $0 == p || rankVotes[$0] == 0 }) {
                rotate(&beats, phase: p)
                return
            }
        }
        if firstChoice != 0, firstChoice == secondChoice {
            offset(&beats, by: -firstChoice)
        }
    }

    private static func changes(_ levels: [Double]) -> [Double] {
        var result = levels
        for i in 1..<levels.count { result[i] = levels[i] - levels[i - 1] }
        let maximum = result.dropLast().max()!
        if abs(maximum) > Double.ulpOfOne {
            result[0] /= maximum
            let reciprocal = 1 / maximum
            for i in 1..<(result.count - 1) { result[i] *= reciprocal }
        }
        return result
    }

    private static func irregularTail(_ changes: [Double]) -> Bool {
        var suffix = changes.map(abs)
        for i in stride(from: suffix.count - 2, through: 0, by: -1) {
            suffix[i] = max(suffix[i], suffix[i + 1])
        }
        let first = (suffix.indices.last { suffix[$0] > 0.7 }).map { $0 + 1 } ?? 0
        let end = changes.count - 2
        guard first < end else { return false }
        let drops = (first..<end).filter { suffix[$0] - suffix[$0 + 1] > 0.005 }
        return drops.count > 4 || zip(drops, drops.dropFirst()).contains { $1 - $0 != 1 }
    }

    /// Eight independent accumulators and fused variance updates preserve the
    /// reduction order used by the vectorized source implementation.
    private static func statistics(_ values: [Double], indices: [Int])
        -> (mean: Double, deviation: Double) {
        let vectorCount = indices.count - indices.count % 8
        var lanes = [Double](repeating: 0, count: 8)
        for i in 0..<vectorCount { lanes[i % 8] += abs(values[indices[i]]) }
        var sum = pairwiseSum(lanes)
        for i in vectorCount..<indices.count { sum += abs(values[indices[i]]) }
        let mean = sum / Double(indices.count)
        lanes = [Double](repeating: 0, count: 8)
        for i in 0..<vectorCount {
            let difference = abs(values[indices[i]]) - mean
            lanes[i % 8] = lanes[i % 8].addingProduct(difference, difference)
        }
        var variance = pairwiseSum(lanes)
        for i in vectorCount..<indices.count {
            let difference = abs(values[indices[i]]) - mean
            variance = variance.addingProduct(difference, difference)
        }
        return (mean, sqrt(variance / Double(indices.count)))
    }

    private static func pairwiseSum(_ lanes: [Double]) -> Double {
        (lanes[6] + (lanes[4] + (lanes[2] + lanes[0])))
            + (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])))
    }

    private static func isolatedVotes(_ values: [Double], beats: [NativeBeatInfo]) -> [Int] {
        let sorted = values.map(abs).sorted(by: >)
        let middle = sorted.count / 2
        let median = sorted.count % 2 == 0
            ? (sorted[middle] + sorted[middle - 1]) * 0.5 : sorted[middle]
        var candidates = (1..<values.count).filter { values[$0] > median + 0.2 }
        if candidates.count < 5 {
            candidates = (1..<values.count).filter { values[$0] > median + 0.1 }
        }
        var isolated: [Int] = []
        for i in candidates.indices {
            if (max(0, i - 4)..<i).allSatisfy({ candidates[i] - candidates[$0] > 4 }) {
                isolated.append(phase(at: candidates[i], beats: beats))
            }
        }
        var votes = [Int](repeating: 0, count: 4)
        if isolated.count >= 2 {
            for phase in isolated where (1...4).contains(phase) { votes[phase - 1] += 1 }
        }
        return votes
    }

    private static func rankedVotes(_ values: [Double], beats: [NativeBeatInfo]) -> [Int] {
        var levels = values
        let last = beats.last!.beatInBar
        var phases = beats.map(\.beatInBar) + [next(last), opposite(last)]
        // Later selection-sort passes cannot change this sixteen-entry prefix.
        // Strict swaps retain the original tie behavior without sorting the tail.
        for i in 0..<16 {
            for j in (i + 1)..<levels.count where levels[i] < levels[j] {
                levels.swapAt(i, j)
                phases.swapAt(i, j)
            }
        }
        var votes = [Int](repeating: 0, count: 4)
        for phase in phases.prefix(16) where (1...4).contains(phase) { votes[phase - 1] += 1 }
        return votes
    }

    private static func firstMaximum(_ values: [Int]) -> Int {
        var result = 0
        for i in 1..<values.count where values[i] > values[result] { result = i }
        return result
    }

    private static func phase(at index: Int, beats: [NativeBeatInfo]) -> Int {
        if index == 0 { return previous(beats[0].beatInBar) }
        if index == beats.count + 1 { return next(beats.last!.beatInBar) }
        return beats[index - 1].beatInBar
    }

    private static func previous(_ value: Int) -> Int { value + (value > 1 ? -1 : 3) }
    private static func next(_ value: Int) -> Int { value < 4 ? value + 1 : value - 3 }
    private static func opposite(_ value: Int) -> Int { value + (value > 2 ? -2 : 2) }

    private static func rotate(_ beats: inout [NativeBeatInfo], phase: Int) {
        for i in beats.indices {
            switch phase {
            case 1: beats[i].beatInBar = previous(beats[i].beatInBar)
            case 2: beats[i].beatInBar = opposite(beats[i].beatInBar)
            case 3: beats[i].beatInBar = next(beats[i].beatInBar)
            default: break
            }
        }
    }

    private static func offset(_ beats: inout [NativeBeatInfo], by offset: Int) {
        for i in beats.indices {
            let value = beats[i].beatInBar + offset
            beats[i].beatInBar = value > 0 ? value : value + 4
        }
    }
}
