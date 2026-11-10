#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Opening evidence is evaluated independently in four streams before voting.
/// All returned positions use the native one-based beat coordinate.
enum NativePhraseOpening {
    struct Result {
        let hypotheses: [Int]
        let selected: Int
        let hasOpening: Bool
        let barHypotheses: [Int]
        let scores: [Double]
    }

    static func judge(envelope: [Double], low: [Double], middle: [Double], vocal: [Double], phase: Int) throws -> Result {
        guard (1...4).contains(phase), envelope.count >= 112, low.count >= 28, middle.count >= 28, vocal.count >= 112 else {
            throw NativePhraseClassifier.InputError.inconsistentDimensions
        }
        let envelopeBar = envelopeCandidate(envelope, phase: phase)
        let vocalBar = vocalCandidate(vocal)
        var lowValues = normalized(low, zeroGuard: false)
        let lowRatios = ratios(lowValues)
        var lowSymmetric = lowRatios.map { $0 < 1 ? 1 / $0 : $0 }
        lowSymmetric[0] = 0
        for i in 1...16 where lowValues[i - 1] < 0.02 && lowValues[i] < 0.02 && lowSymmetric[i] < 3 { lowSymmetric[i] = 1 }
        let lowBar = levelCandidate(&lowValues, ratios: lowRatios, symmetric: lowSymmetric, low: true)
        var middleValues = normalized(middle, zeroGuard: true)
        let middleRatios = ratios(middleValues)
        var middleSymmetric = middleRatios.map { $0 < 1 ? 1 / $0 : $0 }
        middleSymmetric[0] = 0
        let middleBar = levelCandidate(&middleValues, ratios: middleRatios, symmetric: middleSymmetric, low: false)
        let raw = [envelopeBar, vocalBar, lowBar, middleBar]
        return reconcile(raw, low: normalized(low, zeroGuard: false), lowRatios: lowRatios, lowSymmetric: ratios(normalized(low, zeroGuard: false)).map { $0 < 1 ? 1 / $0 : $0 }, phase: phase)
    }

    static func main(envelope: [Double], low: [Double], middle: [Double], vocal: [Double], phase: Int, anchors: [Int]) throws -> Result {
        guard !anchors.isEmpty, (1...4).contains(phase) else { throw NativePhraseClassifier.InputError.inconsistentDimensions }
        let n = min(envelope.count / 4, low.count, middle.count, vocal.count / 4)
        var candidates = [0], selected = 0, found = false, raw = [Int](), scores = [Double]()
        if n >= 8 {
            var e = envelope, l = low, m = middle, v = vocal
            if n < 28 {
                let repeats = (n + 27) / n + 2
                e = Array(repeating: Array(envelope.prefix(n * 4)), count: repeats).flatMap { $0 }
                l = Array(repeating: Array(low.prefix(n)), count: repeats).flatMap { $0 }
                m = Array(repeating: Array(middle.prefix(n)), count: repeats).flatMap { $0 }
                v = Array(repeating: Array(vocal.prefix(n * 4)), count: repeats).flatMap { $0 }
            }
            let result = try judge(envelope: e, low: l, middle: m, vocal: v, phase: phase)
            candidates = result.hypotheses; selected = result.selected; found = result.hasOpening
            raw = result.barHypotheses; scores = result.scores
            if candidates.reduce(0, +) == 0 {
                if n >= 28 { found = true; candidates = [phase]; selected = phase }
                else { found = false }
            } else if n < 28 {
                found = true
                let retained = candidates.map { $0 > n ? 0 : $0 }
                for value in candidates where value <= n { selected = value }
                if retained.reduce(0, +) == 0 { candidates = [phase]; selected = phase }
            }
        }
        candidates.sort(by: >)
        if anchors[0] != 0 {
            var matched = false
            for anchor in anchors.reversed() where candidates.contains(anchor) { selected = anchor; found = true; matched = true }
            if !matched {
                let remainder = ((anchors[0] - phase) / 4 + 1) % 4
                var same = 0
                for candidate in candidates.dropLast() {
                    let difference = candidate - phase
                    let bar = difference / 4 + (difference > 3 ? 1 : 0)
                    if bar % 4 == remainder { same = same == 0 ? candidate : min(same, candidate) }
                }
                if same != 0 { selected = same; found = true }
            }
        }
        return Result(hypotheses: candidates, selected: selected, hasOpening: found, barHypotheses: raw, scores: scores)
    }

    private static func envelopeCandidate(_ envelope: [Double], phase: Int) -> Int {
        let n = (envelope.count - phase + 1) / 4
        var level = [Double](repeating: 0, count: n)
        for i in 0..<(n - 1) { level[i] = sum(envelope, phase - 1 + i * 4, phase + 2 + i * 4) * 0.25 }
        let factor = 1 / (level.max() ?? 0)
        for i in 0..<(n - 1) { level[i] *= factor == 0 ? 0.003 : factor }
        var changes = [Double](repeating: 0, count: 25)
        for i in 1..<25 {
            if level[i - 1] == 0 { level[i - 1] = 0.003 }
            changes[i] = level[i] / level[i - 1]
        }
        let candidates = (0..<25).filter { changes[$0] >= 2 && level[$0] > 0.095 }.map { $0 + 1 }
        var accepted = [Int]()
        for i in candidates {
            let base = safe(level[i - 1])
            let a = safe(sum(level, i, i + 1) * 0.5) / base
            let b = safe(sum(level, i, i + 2) / 3) / base
            if max(a, 1 / a) <= 1.2 && max(b, 1 / b) <= 1.3 { accepted.append(i) }
        }
        var shortDeviation = [Int: Double](), longDeviation = [Int: Double](), priorDeviation = [Int: Double](), contrast = [Int: Double]()
        if accepted.count < candidates.count {
            var short = [Double](repeating: 0, count: 3), long = [Double](repeating: 0, count: 7)
            for i in candidates {
                let shortMean = sum(level, i, i + 2) / 3
                let longMean = sum(level, i, min(i + 6, n - 1)) / Double(min(7, max(1, n - i)))
                for j in 0..<7 where i + j + 1 < n {
                    long[j] = abs(level[i + j] - longMean)
                    if j < 3 { short[j] = abs(level[i + j] - shortMean) }
                }
                shortDeviation[i] = sum(short) / 3; longDeviation[i] = sum(long) / 7
                if shortDeviation[i]! <= 0.1 && longDeviation[i]! <= 0.13 && accepted.last != i { accepted.append(i) }
            }
        }
        if accepted.count < candidates.count {
            for i in candidates {
                let length = min(4, i - 1)
                let prior = Array(level[(i - 1 - length)..<(i - 1)])
                let mean = safe(sum(prior) / Double(prior.count))
                priorDeviation[i] = sum(prior.map { abs($0 - mean) }) / Double(prior.count)
                contrast[i] = level[i - 1] / mean
                if (shortDeviation[i] ?? 0) <= 0.14 && (longDeviation[i] ?? 0) <= 0.16 && priorDeviation[i]! <= 0.04 && contrast[i]! >= 3 && accepted.last != i { accepted.append(i) }
            }
        }
        if accepted.count < candidates.count {
            for i in candidates where (shortDeviation[i] ?? 0) <= 0.18 && (longDeviation[i] ?? 0) <= 0.3 && (priorDeviation[i] ?? 0) <= 0.01 && (contrast[i] ?? 0) >= 3 && accepted.last != i { accepted.append(i) }
        }
        accepted = Set(accepted).sorted()
        guard let first = accepted.first else { return 0 }
        return accepted.count > 1 && accepted[1] == first + 1 ? accepted[1] : first
    }

    private static func vocalCandidate(_ vocal: [Double]) -> Int {
        let maximum = vocal.max() ?? 0
        let v = vocal.prefix(112).map { $0 * (maximum == 0 ? 333.3333333333333 : 1 / maximum) }
        var dense = [Int]()
        for i in 0..<96 where v[i] >= 0.5 && (i % 4 == 0 || v[i + 1] + v[i + 2] + v[i + 3] != 0) {
            let count = v[i...i + 7].filter { $0 > 0 }.count
            let mean = sum(v, i, i + 7) / Double(count)
            if (count >= 5 && mean >= 0.4) || (count == 4 && mean >= 0.5) { dense.append(i + 1) }
        }
        var onset = 0
        for i in 5..<97 {
            if i > 34 && sum(v, 0, i - 1) != 0 { break }
            guard v[i] > 0 else { continue }
            let count = v[i...i + 15].filter { $0 > 0 }.count
            let mean = sum(v, i, i + 15) / 16
            if ((count > 7 && mean > 0.1) || (count > 5 && mean > 0.2)) &&
                (v[i] * v[i + 2] * v[i + 3] > 0 || v[i] * v[i + 1] > 0) && sum(v, 4, i - 1) / Double(i - 4) < 0.05 {
                onset = i + 1; break
            }
        }
        if onset == 0 {
            for i in 8..<34 {
                let counts = (0..<(i / 4)).map { j in v[(j * 4)..<(j * 4 + 4)].filter { $0 > 0 }.count }
                if zip(counts, counts.dropFirst()).contains(where: { $0 >= 3 && $1 >= 3 }) { break }
                guard v[i] > 0 else { continue }
                let nextCounts = (0..<3).map { j in v[(i + j * 4)..<(i + j * 4 + 4)].filter { $0 > 0 }.count }
                let mean = sum(v, i, i + 11) / 12
                if nextCounts.allSatisfy({ $0 > 2 }) && mean > 0.4 && sum(v, 0, i - 1) / Double(i) < mean * 0.5 { onset = i + 1; break }
            }
            if onset == 0 {
                for i in 7..<66 where v[i] > 0 {
                    let count = v[i...i + 7].filter { $0 > 0 }.count
                    if count > 2 && sum(v, i, i + 23) / 24 >= 0.1 && sum(v, 0, i - 1) / Double(i) < 0.04 &&
                        (sum(v, i, i + 7) / Double(count) >= 0.65 || (v[i] > 0.7 && v[i + 1] > 0.3 && v[i + 2] > 0.3)) { onset = i + 1; break }
                }
            }
        }
        func bar(_ beat: Int) -> Int { Int((Double(beat - 1) * 0.25 + 1).rounded(.toNearestOrAwayFromZero)) }
        var result = bar(onset)
        let first = dense.first ?? 0, denseBar = bar(first)
        if (result == 8 && denseBar == 9) || (result == 4 && denseBar == 5) ||
            (!(denseBar == 1 || (result == 5 && denseBar == 4) || (result == 9 && denseBar == 8)) && !(result < denseBar && result != 1)) {
            result = denseBar; onset = first
        }
        if result > 1 {
            if (14...19).contains(onset) { result = 5 }
            if (30...35).contains(onset) { result = 9 }
            if !(14...19).contains(onset) && !(30...35).contains(onset) && (result == 4 || result == 8) && dense.count > 1 && bar(dense[1]) == result + 1 { result += 1 }
        }
        return result
    }

    private static func levelCandidate(_ values: inout [Double], ratios: [Double], symmetric: [Double], low: Bool) -> Int {
        var ranked = (0..<17).sorted { symmetric[$0] == symmetric[$1] ? $0 < $1 : symmetric[$0] > symmetric[$1] }.map { $0 + 1 }
        // The final ranked item is deliberately retained by the native loop.
        ranked = ranked.enumerated().filter { $0.offset == 16 || $0.element > 3 || symmetric[$0.element - 1] >= 1.35 }.map(\.element)
        var candidates = Array(ranked.prefix(5))
        if candidates[1] - candidates[0] == 1 { candidates.remove(at: 1) }
        else if candidates[0] - candidates[1] == 1 { candidates.remove(at: 0) }
        if candidates[2] - candidates[1] == 1 { candidates.remove(at: 2) }
        else if candidates[1] - candidates[0] == 1 { candidates.remove(at: 1) }
        candidates = Array(candidates.prefix(3)).sorted()
        if !low {
            let a = candidates[0], b = candidates[1], c = candidates[2]
            let firstOK = !(a > 2 && b - a == 2 && (safe(symmetric[b - 1]) / safe(symmetric[a - 1]) > 2 || symmetric[b] + symmetric[b - 2] + symmetric[b + 1] < symmetric[a] + symmetric[a - 2] + symmetric[a + 1]))
            let secondOK = !(c - b == 2 && (symmetric[c - 1] / safe(symmetric[b - 1]) > 2 || symmetric[b] + symmetric[b - 2] + symmetric[b + 1] > symmetric[c] + symmetric[c - 2] + symmetric[c + 1]))
            if !firstOK && !secondOK { candidates = [c, b, a] }
            else if !firstOK { candidates = [b, a, c] }
            else if !secondOK { candidates = [a, c, b] }
        }
        for candidate in candidates {
            var accepted = false
            if candidate == 2 {
                accepted = ratios[1] > 1 && (symmetric[2] + symmetric[3]) * 0.5 < 1.4 && symmetric[1] > 1.5
            } else if candidate > 2 {
                let start = ratios[1] >= 0.85 ? 0 : 1
                let preceding = Array(values[start..<(candidate - 1)])
                let mean = sum(preceding) / Double(preceding.count)
                let rawDeviation = sum(preceding.map { abs(mean - $0) }) / Double(preceding.count)
                let deviation = low ? safe(rawDeviation) : rawDeviation
                let current = values[candidate - 1]
                var changed = deviation >= 0.005 ? current / (mean + deviation) > 1.15 || current / (mean - deviation) < 0.8695652173913044 : abs(mean - current) / deviation > 15
                if !changed && candidate > 3 && symmetric[candidate - 1] > 1.3 {
                    let count = candidate < 10 ? 2 : 4
                    let preceding = Array(values[(candidate - 1 - count)..<(candidate - 1)])
                    let mean = sum(preceding) / Double(count)
                    let deviation = sum(preceding.map { abs(mean - $0) }) / Double(count)
                    changed = current / (mean + deviation) > 1.2 || current / (mean - deviation) < 0.8333333333333334
                }
                if changed && candidate + 3 <= 27 {
                    if values[candidate + 2] == 0 { values[candidate + 2] = 0.003 }
                    let a = (symmetric[candidate] + symmetric[candidate + 1]) * 0.5
                    let b = (symmetric[candidate + 1] + symmetric[candidate + 2]) * 0.5
                    let ratio = values[candidate] / values[candidate + 2]
                    accepted = a < 1.4 || b < 1.4 || a < symmetric[candidate - 1] * 0.6 || b < symmetric[candidate - 1] * 0.6 || max(ratio, 1 / ratio) < 1.1
                    if !accepted { continue }
                }
                if !accepted && candidate > 3 {
                    let count = low ? 3 : 2
                    let mean = safe(sum(values, candidate - 1 - count, candidate - 2) / Double(count))
                    let ratio = current / mean
                    // The native conditional comparison accepts unordered
                    // values here (silence produces 0 * infinity upstream).
                    accepted = !(ratio <= 1.4 && ratio >= 0.7)
                }
            }
            if accepted {
                if values[candidate + 1] == 0 { values[candidate + 1] = 0.003 }
                if candidate < 3 && ratios[candidate - 1] <= 1.6 { return candidate }
                if ratios[candidate - 1] <= 1 {
                    return candidate + (ratios[candidate] * ratios[candidate - 1] > 0.83 ? 1 : 0)
                }
                if ratios[candidate] > 1.15 || ratios[candidate] < 0.6 {
                    let ratio = values[candidate - 1] / values[candidate + 1]
                    return candidate + (ratio > 1.05 || ratio < 0.95 ? 1 : 0)
                }
                return candidate
            }
        }
        return 0
    }

    private static func reconcile(_ raw: [Int], low: [Double], lowRatios: [Double], lowSymmetric: [Double], phase: Int) -> Result {
        var hypotheses = raw.map { $0 == 1 ? 0 : $0 }
        var scores = [Double](repeating: 0, count: 4)
        var selected = 0
        for i in 0...1 where hypotheses[i] != 0 && hypotheses.filter({ $0 == hypotheses[i] }).count >= 3 { selected = hypotheses[i]; break }
        if selected == 0 {
            for value in Set(hypotheses).filter({ $0 != 0 }) where hypotheses.filter({ $0 == value }).count == 2 {
                for i in 0..<4 where hypotheses[i] == value { scores[i] += 1 }
            }
            let unique = Set(hypotheses).filter { $0 != 0 }.sorted()
            for i in 0..<4 where hypotheses[i] != 0 {
                let rank = unique.firstIndex(of: hypotheses[i])!
                if rank == 0 { scores[i] += unique.count > 1 && unique[1] - unique[0] > 3 ? 0.7 : 0.4 }
                else if rank == 1 { if unique[1] - unique[0] < 4 { scores[i] += 0.2 } }
                else { scores[i] -= 0.35 }
            }
            let sorted = (0..<4).sorted { hypotheses[$0] == hypotheses[$1] ? $0 < $1 : hypotheses[$0] > hypotheses[$1] }
            for i in 0..<3 where hypotheses[sorted[i]] - hypotheses[sorted[i + 1]] == 1 {
                scores[sorted[i]] += 0.75
                if sorted[i + 1] == 0 { scores[0] += 0.86 }
                else if sorted[i] == 2 && sorted[i + 1] == 3 { scores[2] += 0.3 }
            }
            for i in 0..<4 {
                switch hypotheses[i] {
                case 0: scores[i] = 0
                case 2: scores[i] += 1.2
                case 3: scores[i] += 1.3
                case 5, 17: scores[i] += 1.8
                case 9: scores[i] += 1.85
                case 18...: scores[i] += 0.44999999999999996
                default: scores[i] += 1
                }
            }
        }
        let third = hypotheses[2]
        if third > 0 && !(lowSymmetric[third - 1] >= 1.3) && !((third == 1 ? 0 : abs(low[third - 1] - low[third - 2])) >= 0.09) {
            hypotheses[2] = 0; scores[2] = 0
        }
        if selected == 0 {
            let order = (0..<4).sorted { scores[$0] == scores[$1] ? $0 < $1 : scores[$0] > scores[$1] }
            selected = hypotheses[order[0]]
            let sortedScores = scores.sorted(by: >)
            let unchanged = zip(scores, sortedScores).filter { $0 == $1 }.count
            if raw[2] > 0 && unchanged == 1 && sortedScores[1] == scores[2] && abs(selected - hypotheses[2]) < 5 && scores[order[0]] - scores[2] <= 1.1 && order[0] != 0 {
                let value = lowRatios[raw[2] - 1]
                if !(value >= 0.32 && value <= 1.5) { selected = hypotheses[2] }
            }
        }
        return Result(hypotheses: hypotheses.map { $0 == 0 ? 0 : phase - 4 + $0 * 4 }, selected: selected == 0 ? phase : phase - 4 + selected * 4,
                      hasOpening: true, barHypotheses: raw, scores: scores)
    }

    private static func normalized(_ input: [Double], zeroGuard: Bool) -> [Double] {
        let maximum = input.max() ?? 0
        let factor = zeroGuard && maximum == 0 ? 333.3333333333333 : 1 / maximum
        var result = input
        for i in 0..<(input.count - 1) { result[i] *= factor == 0 ? 0.003 : factor }
        return result
    }
    private static func safe(_ value: Double) -> Double { value == 0 ? 0.003 : value }
    private static func ratios(_ values: [Double]) -> [Double] {
        [0] + (1..<28).map { safe(values[$0]) / safe(values[$0 - 1]) }
    }
    private static func sum(_ values: [Double], _ lower: Int, _ upper: Int) -> Double {
        guard lower <= upper, lower < values.count, upper >= 0 else { return 0 }
        return sum(Array(values[max(0, lower)...min(values.count - 1, upper)]))
    }
    private static func sum(_ values: [Double]) -> Double {
        var lanes = [Double](repeating: 0, count: 8), i = 0
        while i + 8 <= values.count { for j in 0..<8 { lanes[j] += values[i + j] }; i += 8 }
        var value = (lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))) + (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])))
        while i < values.count { value += values[i]; i += 1 }
        return value
    }
}
