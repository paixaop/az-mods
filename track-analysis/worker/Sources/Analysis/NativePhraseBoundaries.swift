#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseBoundaries {
    struct Anchors: Sendable, Equatable {
        let coarseLevels: [Double]
        let differences: [Double]
        /// One-based coarse positions. Zero is the native no-anchor sentinel.
        let positions: [Int]
        let beatPositions: [Int]
        /// A separate early-anchor copy used by opening classification.
        let openingPositions: [Int]
    }

    struct Candidate: Sendable, Equatable {
        let beat: Int
        let subtype: Int
    }

    struct Alternatives: Sendable, Equatable {
        let short: [Candidate]
        let long: [Candidate]
    }

    struct RatioSeries: Sendable, Equatable {
        let levels: [Double]
        let contrast: [Double]
        let direction: [Double]

        func prefix(_ count: Int) -> RatioSeries {
            RatioSeries(levels: Array(levels.prefix(count)), contrast: Array(contrast.prefix(count)),
                direction: Array(direction.prefix(count)))
        }

        func slice(start: Int, count: Int) -> RatioSeries {
            RatioSeries(levels: Array(levels[start..<(start + count)]),
                contrast: Array(contrast[start..<(start + count)]),
                direction: Array(direction[start..<(start + count)]))
        }
    }

    struct CandidateFeatures: Sendable, Equatable {
        let lowPeak: RatioSeries
        let lowLevel: RatioSeries
        let middleCoarsePeak: RatioSeries
        let middleBeatPeak: RatioSeries
    }

    static func candidateFeatures(lowPeaks: [Double], lowLevels: [Double], middlePeaks: [Double],
                                  middleCoarsePeaks: [Double], phase: Int) -> CandidateFeatures {
        let count = max(0, (lowPeaks.count - phase + 1) / 4)
        var low = [Double](repeating: 0, count: count)
        if lowPeaks.count - phase + 1 >= 8 {
            for index in 0..<max(1, count - 1) {
                low[index] = NativePhraseFeatures.sum(lowPeaks, start: phase - 1 + index * 4, count: 4) * 0.25
            }
        }
        return CandidateFeatures(lowPeak: ratioSeries(low), lowLevel: ratioSeries(lowLevels),
            middleCoarsePeak: ratioSeries(middleCoarsePeaks), middleBeatPeak: ratioSeries(middlePeaks))
    }

    static func ratioSeries(_ input: [Double]) -> RatioSeries {
        var levels = input
        var contrast = [Double](repeating: 0, count: input.count)
        var direction = contrast
        let maximum = input.max() ?? 0
        let scale = maximum == 0 ? 333.3333333333333 : 1 / maximum
        if input.count > 1 {
            for index in 0..<(input.count - 1) { levels[index] *= scale }
        }
        if input.count > 2 {
            for index in 1..<(input.count - 1) {
                if levels[index - 1] == 0 { levels[index - 1] = 0.003 }
                if levels[index] == 0 { levels[index] = 0.003 }
                let ratio = levels[index] / levels[index - 1]
                direction[index] = ratio
                contrast[index] = ratio >= 1 ? ratio : 1 / ratio
            }
        }
        return RatioSeries(levels: levels, contrast: contrast, direction: direction)
    }

    /// Local proposal search within a sliding coarse-profile window.
    static func localCandidates(_ profile: RatioSeries, firstBar: Int) -> [Int] {
        let n = profile.levels.count
        guard n >= 10, profile.contrast.count == n, profile.direction.count == n else { return [] }
        let interiorCount = n - 6
        let meanValue = NativePhraseFeatures.sum(profile.contrast, start: 3, count: interiorCount) / Double(interiorCount)
        let mean = meanValue == 0 ? 0.003 : meanValue
        var maximumIndex = 1
        for index in 1..<interiorCount where profile.levels[index + 3] > profile.levels[maximumIndex + 2] {
            maximumIndex = index + 1
        }
        var ranked = (1...interiorCount).sorted {
            let a = profile.contrast[$0 + 2], b = profile.contrast[$1 + 2]
            return a == b ? $0 < $1 : a > b
        }
        let strongest = Array(ranked.prefix(4)).sorted(by: >)
        var retained = strongest
        var replacements = 0
        for index in 0..<3 {
            let position = strongest[index], next = strongest[index + 1]
            if position - next == 1, ranked.count > replacements + 4 {
                let divisor = profile.contrast[next + 2]
                let ratio = divisor == 0 ? profile.contrast[position + 2] * 333.3333333333333
                    : profile.contrast[position + 2] / divisor
                if ratio < 1.7, profile.direction[position + 2] <= 1.8 {
                    retained[index] = 0
                    replacements += 1
                }
                if profile.direction[position + 2] > 1.8 {
                    retained[index + 1] = 0
                    replacements += 1
                }
            }
        }
        ranked.replaceSubrange(0..<4, with: retained)
        var result = [Int](repeating: 0, count: 4 + replacements)
        func meanRange(_ values: [Double], _ start: Int, _ count: Int) -> Double {
            NativePhraseFeatures.sum(values, start: start, count: count) / Double(count)
        }
        func nonzero(_ value: Double) -> Double { value == 0 ? 0.003 : value }
        for (outputIndex, candidate) in ranked.prefix(4 + replacements).enumerated() {
            if candidate == 0 { continue }
            let level = profile.levels[candidate + 2]
            let strength = profile.contrast[candidate + 2]
            let strongestLevel = maximumIndex == candidate
            guard strength > mean * 1.05 || strongestLevel else { continue }
            let levelChange = level / nonzero(meanRange(profile.levels, candidate - 1, 3))
            guard levelChange > mean * 1.09 || levelChange < 0.9174311926605504 / mean || strongestLevel else { continue }
            let ceiling = mean * 1.0573000000000001
            guard (meanRange(profile.contrast, candidate, 2) < ceiling
                && meanRange(profile.contrast, candidate - 1, 3) < ceiling)
                || (strongestLevel && level / nonzero(profile.levels[candidate + 1]) > 1.1) else { continue }
            let following = nonzero(meanRange(profile.contrast, candidate + 3, 2))
            let followingNext = nonzero(meanRange(profile.contrast, candidate + 4, 2))
            if following >= 1.2, strength / following <= mean,
               followingNext >= 1.2, strength / followingNext <= mean,
               strength <= 1.4 || (profile.contrast[candidate + 5] + profile.contrast[candidate + 3]) * 0.5 >= 1.2 {
                continue
            }
            result[outputIndex] = firstBar - 1 + candidate + 3
        }
        while result.last == 0 { result.removeLast() }
        if result.isEmpty { return [0] }
        result.sort()
        for _ in 0..<3 where result.first == 0 { result.removeFirst() }
        return result.first == 0 ? [0] : result
    }

    static func verseHookCandidates(features: CandidateFeatures, openingHypotheses: [Int],
                                    introEnd: Int, phase: Int, route: Int) -> [Int] {
        let n = min(features.lowPeak.levels.count, features.lowLevel.levels.count,
            features.middleCoarsePeak.levels.count)
        guard n > 0 else { return [] }
        let profiles = [features.lowPeak.prefix(n), features.lowLevel.prefix(n), features.middleCoarsePeak.prefix(n)]
        var seeds = openingHypotheses.map { position -> Int in
            let coarse = Int(Double(position - phase) * 0.25 + 1.5)
            return coarse <= n ? coarse : 0
        }.sorted()
        if seeds.count > 1 {
            for index in 0..<(seeds.count - 1) where seeds[index] == seeds[index + 1] { seeds[index] = 0 }
        }
        seeds.sort()
        var removed = 0
        while removed < 4, seeds.first.map({ $0 < 1 }) == true { seeds.removeFirst(); removed += 1 }
        if seeds.isEmpty || seeds.reduce(0, +) == 0 { seeds = [0] }
        let intro = Int(Double(introEnd - phase) * 0.25 + 1.5)
        if 4 - removed >= 2, let first = seeds.first, intro - first == 1 { seeds.removeFirst() }
        if seeds.isEmpty { seeds = [0] }
        let last = max(seeds.last ?? 0, 3)
        var end = min(last + 19, n)
        var start = min(last - 2, end)
        var additions = [Int]()
        if n - last - 1 > -16 {
            let iterations = (n - last - 1) / 16 + 1
            if iterations > 0 {
                for _ in 0..<iterations {
                    let length = end - start + 1
                    if start >= 1, length >= 0, start - 1 + length <= n {
                        for (index, profile) in profiles.enumerated() where route != 1 || index == 1 {
                            additions += localCandidates(profile.slice(start: start - 1, count: length), firstBar: start)
                        }
                    }
                    if n - end < 10 { break }
                    end += 16
                    if end >= n { end = n - 1 }
                    start += 16
                }
            }
        }
        additions = Array(Set(additions.filter { $0 != 0 })).sorted()
        let combined = seeds + additions
        var surviving = combined
        if combined.count > 1 {
            for index in 0..<(combined.count - 1) where combined[index] != 0 && combined[index] == combined[index + 1] - 1 {
                surviving[index] = 0
            }
        }
        surviving = surviving.filter { $0 != 0 }.sorted()
        if surviving.first != 1 { surviving.insert(1, at: 0) }
        let original = surviving
        var scores = surviving.map { position -> Double in
            let index = position - 1
            guard index >= 0, index < n else { return 0 }
            let score = (profiles[2].contrast[index] * 0.9)
                .addingProduct(profiles[1].contrast[index] + profiles[0].contrast[index], 1.05)
            if position == 1 { return 0 }
            return position > 2 ? (profiles[1].contrast[index - 1] + score) * 0.25 : score * 0.3333333333333333
        }
        if surviving.count > 2 {
            for index in 2..<surviving.count {
                if scores[index - 1] == 0 { scores[index - 1] = 0.003 }
                if scores[index] == 0 { scores[index] = 0.003 }
                let previous = scores[index - 1], current = scores[index]
                let ratio = max(current / previous, previous / current)
                if ratio >= 1.07, original[index] - original[index - 1] < 4 {
                    let remove = current <= previous ? index : index - 1
                    let position = surviving[remove]
                    if position != 5, position != 9, position != intro, scores[remove] < 1.6 {
                        surviving[remove] = 0
                    }
                }
            }
        }
        return surviving.filter { $0 != 0 }.map { phase - 4 + $0 * 4 }
    }

    /// Beat-profile break proposals, returned in the native coarse coordinate.
    static func shortBreakCandidates(lowLevels: [Double], middlePeaks: [Double], count: Int,
                                     barCount: Int) -> [Int] {
        guard count > 0, barCount > 0 else { return [] }
        let scale = (lowLevels.max() ?? 0) == 0 ? 333.3333333333333 : 1 / lowLevels.max()!
        var low = Array(lowLevels.map { $0 * scale }.prefix(count))
        var middle = Array(middlePeaks.prefix(count))
        low += repeatElement(0, count: max(0, count - low.count))
        middle += repeatElement(0, count: max(0, count - middle.count))
        var lowRatio = [Double](repeating: 0, count: count)
        var middleRatio = lowRatio, lowFalls = lowRatio, middleFalls = lowRatio
        var lowRiseCount = 0, middleRiseCount = 0
        if count >= 6 {
            for index in 1..<(count - 4) {
                if low[index] == 0 { low[index] = 0.003 }
                if low[index - 1] == 0 { low[index - 1] = 0.003 }
                lowRatio[index] = low[index] / low[index - 1]
                if lowRatio[index] >= 1 { lowRiseCount += 1 }
                else { lowFalls[index] = lowRatio[index] }
                if middle[index] == 0 { middle[index] = 0.003 }
                if middle[index - 1] == 0 { middle[index - 1] = 0.003 }
                middleRatio[index] = middle[index] / middle[index - 1]
                if middleRatio[index] >= 1 { middleRiseCount += 1 }
                else { middleFalls[index] = middleRatio[index] }
            }
        }
        var lowThreshold = (lowRiseCount == 0 ? 0.003 : NativePhraseFeatures.sum(lowFalls, start: 0, count: count) / Double(lowRiseCount)) * 0.65
        let middleThreshold = (middleRiseCount == 0 ? 0.003 : NativePhraseFeatures.sum(middleFalls, start: 0, count: count) / Double(middleRiseCount)) * 0.95
        let sorted = lowRatio.filter { $0 != 0 }.sorted()
        let smallCount = sorted.prefix { $0 > 0 && $0 <= lowThreshold }.count
        if smallCount > 13 { lowThreshold = sorted[12] }
        var candidates = [Int]()
        if count > 20 {
            for index in 16..<(count - 4) {
                if lowRatio[index] < lowThreshold, middleRatio[index] < middleThreshold,
                   (low[index - 1] > 0.5 && (middle[index - 1] > 0.2 || middle[index - 2] > 0.2))
                    || (low[index - 1] > 0.2 && (middle[index - 1] > 0.5 || middle[index - 2] > 0.5)) {
                    candidates.append(index + 1)
                }
            }
        }
        candidates = candidates.filter { position in
            if position % 4 == 1 || position % 4 == 2 { return false }
            let scale = low[position - 1] == 0 ? 333.3333333333333 : 1 / low[position - 1]
            return (position..<(position + 4)).contains { low[$0] * scale > 2.5 }
        }.sorted()
        if candidates.isEmpty { return [Int](repeating: 0, count: barCount) }
        return candidates.map { $0 / 4 + ($0 % 4 == 0 ? 1 : 2) }
    }

    /// Merge beat-coordinate candidates with independent coarse proposals.
    static func adjustCandidates(_ base: [Int], additions: [Int], phase: Int, barCount: Int) -> [Int] {
        var extra = additions
        if additions.count > 1 {
            for index in 0..<(additions.count - 1) where additions[index + 1] == additions[index] + 1 {
                extra[index] = 0
            }
        }
        extra = extra.filter { $0 != 0 }.sorted()
        let coarse = base.map { ($0 - phase) / 4 + 1 }
        for position in coarse.dropFirst() {
            for index in extra.indices where extra[index] == position || extra[index] == position - 1 || extra[index] == position + 1 {
                extra[index] = 0
            }
        }
        return (coarse + extra).filter { $0 != 0 && $0 <= barCount - 4 }.sorted().map { phase - 4 + $0 * 4 }
    }

    static func addGenre23Candidates(base: [Int], lowLevels: [Double], middlePeaks: [Double],
                                    souon: [Int], snare: [Int], vocal: [Int], phase: Int) -> [Int] {
        let count = middlePeaks.filter { $0 != 0 }.count
        let bars = count / 4
        let breaks = shortBreakCandidates(lowLevels: lowLevels, middlePeaks: middlePeaks,
            count: count, barCount: bars)
        let extra = Array(Set((breaks + souon + snare + vocal).filter { $0 != 0 })).sorted()
        return adjustCandidates(base, additions: extra, phase: phase, barCount: bars)
    }

    static func securePoints(lowPeaks: [Double], phase: Int) -> Anchors {
        let count = lowPeaks.count / 4
        var levels = [Double](repeating: 0, count: count)
        var differences = levels
        if lowPeaks.count >= 8 {
            for index in 0..<max(1, count - 1) {
                levels[index] = NativePhraseFeatures.sum(lowPeaks, start: phase - 1 + index * 4, count: 4) * 0.25
            }
            for index in 0..<max(1, count - 1) { differences[index] = levels[index + 1] - levels[index] }
        }
        let threshold = (differences.max() ?? 0) * 0.6
        var positions = [Int]()
        if lowPeaks.count >= 12 {
            for index in 0..<(max(3, count) - 2) where differences[index] > threshold && differences[index] > 0.15 {
                let candidate = index + 1
                let minimum = levels[candidate] * 0.7
                let maximum = levels[candidate] * 1.3
                // Short-circuiting here matters at the terminal default slot.
                if levels[candidate + 1] >= minimum, levels[candidate + 1] <= maximum,
                   candidate + 2 < levels.count,
                   levels[candidate + 2] >= minimum, levels[candidate + 2] <= maximum {
                    positions.append(candidate + 1)
                }
            }
        }
        if positions.isEmpty { positions = [0] }
        let beats = positions.map { $0 == 0 ? 0 : phase - 4 + 4 * $0 }
        // The caller retains positions 1...199 in this copy. Other copies
        // remain intact for proposal construction and later reconciliation.
        let opening = positions == [0] ? [0] : beats.filter { $0 > 0 && $0 < 200 }
        return Anchors(coarseLevels: levels, differences: differences, positions: positions,
            beatPositions: beats, openingPositions: opening)
    }

    /// Initial Mid-context proposals only. Subtypes are internal route values;
    /// later candidate adjustment and role classification consume these arrays.
    static func initialAlternatives(anchors: Anchors, phase: Int) -> Alternatives {
        guard anchors.positions.first != 0, !anchors.positions.isEmpty else {
            return Alternatives(short: [], long: [])
        }
        func build(precedingBeats: Int, firstThreshold: Int, gapThreshold: Int) -> [Candidate] {
            var result = [Candidate(beat: 1, subtype: 10)]
            var previous = 0
            for (index, coarse) in anchors.positions.enumerated() {
                let beat = phase - 4 + coarse * 4
                if index == 0 ? coarse >= firstThreshold : coarse - previous >= gapThreshold {
                    result.append(Candidate(beat: beat - precedingBeats, subtype: 30))
                }
                result.append(Candidate(beat: beat, subtype: 40))
                previous = coarse
            }
            return result
        }
        return Alternatives(short: build(precedingBeats: 32, firstThreshold: 10, gapThreshold: 9),
            long: build(precedingBeats: 64, firstThreshold: 18, gapThreshold: 17))
    }
}
