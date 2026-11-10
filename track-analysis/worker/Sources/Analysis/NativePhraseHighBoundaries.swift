#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativePhraseBoundaries {
    struct BreakEvidence: Equatable {
        var start = 0
        var end = 0
        var deep = false
    }

    struct ChangeEvidence: Equatable {
        var firstAligned = 0
        var first = 0
        var secondAligned = 0
        var second = 0
    }

    static func lowPeakCandidates(introEnd: Int, changes: [Double], phase: Int,
                                  lowLevelChanges: [UInt8]) -> [Int] {
        let extent = lowLevelChanges.count * 4
        guard extent > 0, phase > 0, phase <= changes.count else { return [] }
        let flags = lowLevelChanges.flatMap { Array(repeating: $0, count: 4) }
        var aligned = Array(changes.dropFirst(phase - 1).prefix(extent))
        aligned += repeatElement(0, count: max(0, extent - aligned.count))
        var result = [introEnd], last = introEnd, attempt = 1
        let preferredGaps = [32, 64, 16, 96, 48, 80]
        while true {
            let gap = attempt <= 6 ? preferredGaps[attempt - 1] : attempt * 16
            let proposed = last + gap
            if proposed < extent {
                let index = proposed - phase
                if index >= 1, index < extent,
                   aligned[index] == 1 || aligned[index - 1] == 1 || flags[index] == 1 {
                    result.append(proposed)
                    last = proposed
                    attempt = 1
                } else { attempt += 1 }
            } else { attempt += 1 }
            if proposed >= extent - 4, attempt >= 7 { break }
        }
        return result.filter { $0 != 0 }
    }

    static func breakEvidence(candidates: [Int], phase: Int, lowLevels: [Double],
                              middleLevels: [Double], allLevels: [Double]) -> BreakEvidence {
        let count = min(lowLevels.count, middleLevels.count, allLevels.count)
        var low = lowLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        var middle = middleLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        var all = allLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        var result = BreakEvidence()
        guard candidates.count > 2 else { return result }
        for position in candidates.dropFirst().dropLast() {
            let index = position - phase
            guard index >= 1, index < count * 4 else { continue }
            if low[index - 1] == 0 { low[index - 1] = 0.003 }
            if middle[index - 1] == 0 { middle[index - 1] = 0.003 }
            if all[index - 1] == 0 { all[index - 1] = 0.003 }
            let middleRatio = middle[index] / middle[index - 1]
            guard low[index] / low[index - 1] < 0.35, middleRatio < 0.65,
                  all[index] / all[index - 1] < 0.8 else { continue }
            if middleRatio < 0.3 { result.deep = true }
            for distance in [4, 8, 12] {
                let next = index + distance
                guard next < count * 4 else { continue }
                if low[next - 1] == 0 { low[next - 1] = 0.003 }
                if middle[next - 1] == 0 { middle[next - 1] = 0.003 }
                if all[next - 1] == 0 { all[next - 1] = 0.003 }
                let recovery = middle[next] / middle[next - 1]
                if low[next] / low[next - 1] > 2.7, recovery > 1.3,
                   all[next] / all[next - 1] > 1.2 {
                    if result.deep, recovery < 2.5 { result.deep = false }
                    result.start = position; result.end = position + distance
                    return result
                }
            }
        }
        return result
    }

    static func changeEvidence(candidates: [Int], phase: Int, lowLevels: [Double],
                               middleLevels: [Double], allLevels: [Double],
                               openingEnvelope: [Double], lowPeaks: [Double]) -> ChangeEvidence {
        guard let first = candidates.first, phase >= 1, phase <= lowPeaks.count else { return ChangeEvidence() }
        var peaks = Array(lowPeaks.dropFirst(phase - 1))
        var opening = openingEnvelope
        let count = min(lowLevels.count, middleLevels.count, allLevels.count,
            Int(Double(peaks.count) * 0.25 + 0.5), Int(Double(opening.count) * 0.25 + 0.5))
        var low = lowLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        var middle = middleLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        var all = allLevels.prefix(count).flatMap { Array(repeating: $0, count: 4) }
        let start = first - phase + 1
        let end = count - Int(Double(start) * 0.25 + 0.5) - 8
        guard end > 0 else { return ChangeEvidence() }
        var found = [Int]()
        for bar in 1...end {
            let delta = bar * 4
            let index = start + delta - 1
            guard index >= 1, index < min(low.count, opening.count, peaks.count) else { continue }
            if low[index - 1] == 0 { low[index - 1] = 0.003 }
            if middle[index - 1] == 0 { middle[index - 1] = 0.003 }
            if all[index - 1] == 0 { all[index - 1] = 0.003 }
            if peaks[index - 1] == 0 { peaks[index - 1] = 0.003 }
            if opening[index] == 0 { opening[index] = 0.018 }
            if opening[index - 1] == 0 { opening[index - 1] = 0.018 }
            let a = low[index] / low[index - 1]
            let b = middle[index] / middle[index - 1]
            let c = all[index] / all[index - 1]
            let d = peaks[index] / peaks[index - 1]
            let e = opening[index] / opening[index - 1]
            let qualifies = (a > 20 && b > 1.4 && c > 1.3)
                || (d > 1.2 && a > 2.5 && b > 2.5 && c > 1.7 && e > 3)
                || (d > 6.5 && a > 8.5 && b < 0.95 && e > 1)
                || (b > 2.7 && c > 2.5 && e > 1.5)
                || (d < 0.1 && a < 0.1 && b > 1 && c < 0.8 && e < 0.8)
                || (d > 4 && a > 2.5 && b > 1.3 && c < 1 && e < 1)
            guard qualifies else { continue }
            if found.isEmpty, candidates.count > 1,
               delta % 16 == 0 || candidates.dropFirst().contains(first + delta) { continue }
            found.append(index + phase)
            if found.count == 2 { break }
        }
        var result = ChangeEvidence()
        if let position = found.first {
            result.first = position; result.firstAligned = (position - first) / 16 * 16 + first
        }
        if found.count > 1 {
            result.second = found[1]; result.secondAligned = (found[1] - first) / 16 * 16 + first
        }
        return result
    }
    static func irregularSeeds(introEnd: Int, changes: [Double], phase: Int,
                               lowLevelChanges: [UInt8], longGapStart: Int,
                               breakInfo: BreakEvidence, changeInfo: ChangeEvidence) -> [Int] {
        func candidateCount(_ start: Int) -> Int {
            lowPeakCandidates(introEnd: start, changes: changes, phase: phase,
                lowLevelChanges: lowLevelChanges).count
        }
        let baseline = longGapStart
        var selected = baseline, second = 0
        if breakInfo.start != 0 {
            let before = candidateCount(breakInfo.start)
            let after = candidateCount(breakInfo.end) + (breakInfo.deep ? 2 : 0)
            if before <= after { selected = breakInfo.end }
        }
        if changeInfo.firstAligned != 0, selected != changeInfo.first,
           candidateCount(changeInfo.firstAligned) <= candidateCount(changeInfo.first) {
            if selected != baseline { second = changeInfo.first }
            else { selected = changeInfo.first }
        }
        if selected != changeInfo.second, changeInfo.secondAligned != 0, second == 0,
           candidateCount(changeInfo.secondAligned) <= candidateCount(changeInfo.second) {
            if selected == baseline { selected = changeInfo.second }
            else { second = changeInfo.second }
        }
        if baseline != 0, selected == baseline {
            let flags = lowLevelChanges.flatMap { Array(repeating: $0, count: 4) }
            func eligible(_ position: Int) -> Bool {
                let index = position - phase
                guard position > 1, position - 1 < changes.count, index >= 0, index < flags.count else { return false }
                return changes[position - 1] > 0 || changes[position - 2] > 0 || flags[index] != 0
            }
            var bestCount = eligible(baseline) ? candidateCount(baseline) : 0
            for delta in [4, 8, 12] where eligible(baseline + delta) {
                let count = candidateCount(baseline + delta)
                if count > bestCount { selected = baseline + delta }
                bestCount = max(bestCount, count)
            }
        }
        var result = [introEnd]
        if selected != baseline { result.append(selected) }
        if second != 0 { result.append(second) }
        return result
    }

    struct VerseProfile {
        var lowPeaks: [Double]
        var lowLevels: [Double]
        var middleLevels: [Double]
        var openingEnvelope: [Double]
        var allLevels: [Double]
        var lowChanges: [Double]
    }

    static func verseScore(position: Int, profile: inout VerseProfile, weight: Double) -> Double {
        let bar = Int(Double(position) * 0.25 + 0.5)
        precondition(position >= 5 && position <= profile.lowPeaks.count && position <= profile.openingEnvelope.count
            && position <= profile.lowChanges.count && bar >= 1 && bar < profile.lowLevels.count
            && bar < profile.middleLevels.count && bar < profile.allLevels.count)
        for index in (position - 3)..<position where profile.lowPeaks[index] == 0 { profile.lowPeaks[index] = 0.003 }
        for index in (bar - 1)...bar {
            if profile.lowLevels[index] == 0 { profile.lowLevels[index] = 0.003 }
            if profile.middleLevels[index] == 0 { profile.middleLevels[index] = 0.003 }
            if profile.allLevels[index] == 0 { profile.allLevels[index] = 0.003 }
        }
        for index in (position - 2)..<position where profile.openingEnvelope[index] == 0 { profile.openingEnvelope[index] = 0.003 }
        let changes = profile.lowChanges
        let index = changes[position - 1] == 0 && changes[position - 2] != 0 ? position - 2 : position - 1
        var peakRatio = profile.lowPeaks[index] / profile.lowPeaks[index - 1]
        if changes[index] == 0.8 || changes[index] == 0.4 { peakRatio *= 0.7 }
        func contrast(_ ratio: Double) -> Double { ratio >= 1 ? ratio : 1 / ratio }
        var peakContrast = contrast(peakRatio)
        if (changes[position - 4] + changes[position - 5]) + (changes[position - 3] + changes[position - 2]) == 3 {
            peakContrast *= 1.2
        }
        let low = contrast(profile.lowLevels[bar] / profile.lowLevels[bar - 1])
        let middle = contrast(profile.middleLevels[bar] / profile.middleLevels[bar - 1])
        let opening = contrast(profile.openingEnvelope[position - 1] / profile.openingEnvelope[position - 2])
        let all = contrast(profile.allLevels[bar] / profile.allLevels[bar - 1])
        return ((low + peakContrast) + (middle + opening) + all) * weight
    }

    static func allBandCandidates(seeds: [Int], phase: Int, lowChanges: [Double],
                                  lowLevelChanges: [UInt8], middleLevelChanges: [UInt8],
                                  hihatChanges: [UInt8], profile: VerseProfile) -> [Int] {
        guard !seeds.isEmpty, phase >= 1, phase <= lowChanges.count else { return [] }
        let barCount = min(lowLevelChanges.count, middleLevelChanges.count, hihatChanges.count,
            Int(Double(lowChanges.count) * 0.25 + 0.5), Int(Double(profile.openingEnvelope.count) * 0.25 + 0.5))
        let count = barCount * 4
        guard count > 0 else { return [] }
        let masks = [lowLevelChanges, middleLevelChanges, hihatChanges].map {
            $0.prefix(barCount).flatMap { Array(repeating: $0, count: 4) }
        }
        var local = profile
        local.lowChanges = Array(lowChanges.dropFirst(phase - 1).prefix(count))
        local.lowChanges += repeatElement(0, count: max(0, count - local.lowChanges.count))
        let changes = local.lowChanges
        func eligible(_ position: Int) -> Bool {
            guard position >= 2, position <= count else { return false }
            return changes[position - 1] == 1 || changes[position - 2] == 1
                || masks.contains { $0[position - 1] == 1 }
        }
        let limits = seeds.map { $0 + 1 - phase } + [count]
        let preferredGaps = [32, 64, 16, 96, 48, 80, 128, 112]
        var previousAttempt = 1
        var result = [Int]()
        for segment in 0..<seeds.count {
            var current = limits[segment]
            let end = limits[segment + 1]
            result.append(current)
            var attempt = 1
            while attempt < 12 {
                let gap = attempt <= 8 ? preferredGaps[attempt - 1] : attempt * 16
                var proposed = current + gap
                if proposed >= end || previousAttempt >= end - 4 {
                    if previousAttempt < end - 4 { previousAttempt = proposed }
                    break
                }
                if eligible(proposed) {
                    if attempt == 1 || attempt == 2 {
                        let earlier = current + (attempt == 1 ? 16 : 48)
                        if eligible(earlier) {
                            let earlyScore = verseScore(position: earlier, profile: &local, weight: 1)
                            let weight = attempt == 2 ? 1.02 : (phase - 1 + proposed <= count / 2 ? 1.25 : 1.23)
                            let laterScore = verseScore(position: proposed, profile: &local, weight: weight)
                            if earlyScore > laterScore { proposed = earlier }
                        }
                    }
                    current = proposed
                    result.append(proposed)
                    attempt = 1
                } else { attempt += 1 }
                previousAttempt = proposed
            }
        }
        return result.map { $0 + phase - 1 }
    }

    static func adjustHighCandidates(_ candidates: [Int], allLevels: [Double], lowLevels: [Double]) -> [Int] {
        let count = allLevels.count * 4
        var all = [Double](repeating: 0, count: count), low = all
        if allLevels.count > 1 {
            for bar in 0..<(allLevels.count - 1) {
                for beat in 0..<4 { all[bar * 4 + beat] = allLevels[bar]; low[bar * 4 + beat] = lowLevels[bar] }
            }
        }
        var output = candidates
        if candidates.count > 1 {
            for index in 1..<candidates.count {
                let position = candidates[index]
                if position > count - 16 { output[index] = 0; continue }
                guard position >= 2, position <= count else { continue }
                if all[position - 1] <= 0.32, all[position - 2] <= 0.32 { output[index] = 0 }
                if all[position - 1] >= 0.32, all[position - 1] < 0.56,
                   all[position - 2] >= 0.32, all[position - 2] < 0.56,
                   low[position - 1] <= 300, low[position - 2] <= 300 { output[index] = 0 }
            }
        }
        output = output.filter { $0 > 0 && $0 <= count }
        return output.isEmpty ? [0] : output
    }

    struct RhythmRejection {
        let candidates: [Int]
        let scores: [Double]
        let flags: [UInt8]
        let mean: Double
    }

    static func rejectRepeatedRhythm(_ candidates: [Int], rhythm: [UInt8]) -> RhythmRejection {
        var positions = [Int](), scores = [Double]()
        var flags = [UInt8](repeating: 0, count: candidates.count)
        if candidates.count > 1 {
            for i in 1..<candidates.count {
                let beat = candidates[i]
                if Double(rhythm.count) * 0.25 - Double(beat) < 32 { break }
                guard beat - candidates[i - 1] == 32 else { continue }
                let center = beat * 4
                guard center >= 121, center + 110 < rhythm.count else { continue }
                var matches = 0
                for offset in 7...46 where rhythm[center + offset - 128] == rhythm[center + offset] { matches += 1 }
                for offset in 71...110 where rhythm[center + offset - 128] == rhythm[center + offset] { matches += 1 }
                if i != 1, i != candidates.count - 1 {
                    var nearby = 0, earlier = 0
                    for offset in -49 ... -34 where rhythm[center + offset] == rhythm[center + offset + 32] { nearby += 1 }
                    for offset in -113 ... -98 where rhythm[center + offset] == rhythm[center + offset + 64] { earlier += 1 }
                    if nearby < earlier { flags[positions.count] = 1 }
                }
                positions.append(beat)
                scores.append(Double(matches) * 0.0125)
            }
        }
        let mean = scores.isEmpty ? 0 : NativePhraseFeatures.sum(scores, start: 0, count: scores.count) / Double(scores.count)
        var result = candidates
        for i in positions.indices {
            let indices = result.indices.filter { result[$0] == positions[i] }
            guard !indices.isEmpty, indices.allSatisfy({ $0 > 0 && result[$0] - result[$0 - 1] == 32 }) else { continue }
            let reject = i == 0 ? (flags[0] == 0 && mean < scores[0]) : ((flags[i] == 0 ? 1.02 : 1.06) * mean < scores[i])
            if reject {
                for index in indices { result.remove(at: index) }
            }
        }
        return RhythmRejection(candidates: result, scores: scores, flags: flags, mean: mean)
    }

    /// Genre-one reconciliation works in one-based four-beat ordinals until
    /// its final conversion. Short-break and snare inputs already use these ordinals.
    static func addGenre1Candidates(base: [Int], secondary: [Int], lowProfile: RatioSeries,
                                    shortBreaks: [Int], snares: [Int], phase: Int) -> [Int] {
        func coarse(_ beats: [Int]) -> [Int] {
            var values = beats.filter { $0 != 0 }.sorted().map { beat in
                let value = Double(beat - phase) * 0.25
                return Int(value + (value >= 0 ? 0.5 : -0.5)) + 1
            }
            while values.first == 0 { values.removeFirst() }
            return values
        }
        let primary = coarse(base)
        var additions = coarse(secondary)
        for index in additions.indices {
            var snapped = false
            for existing in primary {
                let position = additions[index]
                guard abs(position - existing) == 1,
                      position > 0, position <= lowProfile.levels.count else { continue }
                if lowProfile.contrast[position - 1] < 1.5 || lowProfile.levels[position - 1] < 0.5 {
                    snapped = true
                    additions[index] = existing
                } else if !snapped, existing - position == 1,
                          lowProfile.direction[position - 1] <= 0.5,
                          lowProfile.direction[existing - 1] >= 1.8 {
                    additions[index] = existing
                }
            }
        }
        let distinct = Set(additions + shortBreaks + snares).subtracting(primary).subtracting([0])
        let combined = (primary + distinct).filter { $0 != 0 }.sorted()
        var result = combined
        var removed = 0
        if combined.count > 1 {
            for originalIndex in 1..<combined.count {
                let index = originalIndex - removed
                let position = result[index]
                let previousGap = position - result[index - 1]
                guard distinct.contains(combined[originalIndex]), previousGap % 4 != 0,
                      originalIndex != combined.count - 1 else { continue }
                let nextGap = result[index + 1] - position
                let direction = lowProfile.direction[position - 1]
                if previousGap >= 4, previousGap % 4 == 3,
                   nextGap >= 4, nextGap % 4 == 1, direction <= 0.83,
                   lowProfile.direction[position] * direction >= 0.9 {
                    result[index] += 1
                } else if previousGap == 1 || nextGap == 1 || direction > 0.83 ||
                            ((previousGap == 2 || previousGap % 4 == 2) && nextGap == 2) {
                    result.remove(at: index)
                    removed += 1
                }
            }
        }
        if let last = result.last, last > lowProfile.levels.count - 4 { result.removeLast() }
        return result.map { phase - 4 + $0 * 4 }
    }

    static func reconcileHighSeeds(_ seeds: [Int], anchors: [Int]) -> [Int] {
        guard let first = anchors.first, first >= 1 else { return seeds }
        var result = seeds, remaining = anchors
        for index in result.indices.reversed() {
            for anchorIndex in remaining.indices.reversed() {
                if result[index] == remaining[anchorIndex] {
                    remaining.remove(at: anchorIndex)
                } else if abs(result[index] - remaining[anchorIndex]) == 4 {
                    result[index] = 0
                }
            }
        }
        return (result + remaining).filter { $0 != 0 }.sorted()
    }

    static func finishCandidates(_ candidates: [Int], initialBoundary: Int,
                                  anchors: [Int], route: Int, endBeat: Int) -> [Int] {
        var result = candidates
        if route != 1 {
            while let last = result.last, last > endBeat { result.removeLast() }
        }
        while let first = result.first, first <= initialBoundary + 3 { result.removeFirst() }
        result.insert(initialBoundary, at: 0)
        if route == 1 {
            for anchor in anchors.reversed() {
                if result.contains(anchor) { continue }
                if let preceding = result.lastIndex(where: { $0 < anchor }) {
                    result.insert(anchor, at: preceding + 1)
                }
            }
            for anchor in anchors.reversed() {
                for index in result.indices.dropFirst().reversed()
                    where result[index] != anchor && abs(result[index] - anchor) <= 8 {
                    result.remove(at: index)
                }
            }
        }
        if result.count > 1, result[0] == result[1] { result.removeFirst() }
        return result
    }

}
