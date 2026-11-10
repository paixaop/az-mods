#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// The classifier's numerical domain is beat-indexed native analysis streams.
/// Keep its inputs distinct from display waveform statistics and onset scores.
enum NativePhraseClassifier {
    enum InputError: Error { case inconsistentDimensions, unsupportedRange }

    struct MoodInputs {
        var slowRock = 0
        var slowTempo = 0
        var lowPowerRatio = 0.0
        var popRock = 0
        var alternateRock = 0
        var sparseKick = 0
        var fourKick = 0
        var fadeOut = 0
        var firstOverride = 0
        var secondOverride = 0
        var narrowBPM = 0
        var broadBPM = 0
        var typicalGenre = 0
        var compression = 0.0
    }

    static func initialMood(_ x: MoodInputs) -> PhraseMood {
        guard x.lowPowerRatio >= 0.3979, x.slowRock != 1,
              x.compression >= 0.6128 || x.slowTempo != 1 else { return .low }
        guard x.fadeOut != 1, x.firstOverride != 1 else { return .mid }
        if x.secondOverride == 1 { return .high }
        var score = Double(x.alternateRock) * -0.05
        score = score.addingProduct(Double(x.narrowBPM) - Double(x.sparseKick), 0.3)
        score = score.addingProduct(Double(x.typicalGenre) + Double(x.broadBPM), 0.1)
        score = score.addingProduct(Double(x.fourKick) - Double(x.popRock) + x.compression, 0.6)
        return score > 1 ? .high : .mid
    }

    static func bpmFlags(_ bpm: Double) -> (narrow: Int, broad: Int) {
        if (127.04...128.96).contains(bpm) || (132.330025...134.32997500000002).contains(bpm) {
            return (1, 0)
        }
        return (0, (122.88...133.12).contains(bpm) || (127.99680000000001...138.66320000000002).contains(bpm) ? 1 : 0)
    }

    static func typicalGenre(bpm: Double, rhythmType: Int) -> Int {
        (96.1...105.01).contains(bpm) && rhythmType == 1 ? 1 : 0
    }

    struct LowPower {
        let ratio: Double
        let blockRatios: [Double]
        let blockPowers: [Double]
        let maximumRatioMean: Double
        let maximumPowerMean: Double
    }

    static func lowPower(low: [Double], full: [Double], beatCount: Int) throws -> LowPower {
        guard low.count == full.count, beatCount >= 0, low.count >= (beatCount / 4) * 16 else {
            throw InputError.inconsistentDimensions
        }
        let denominator = sum(full)
        let ratio = denominator > 0 ? sqrt(sum(low) / denominator) : 1
        let count = beatCount / 4
        guard count > 0 else {
            return LowPower(ratio: ratio, blockRatios: [0], blockPowers: [0], maximumRatioMean: 0, maximumPowerMean: 0)
        }
        var ratios = [Double](), powers = [Double]()
        for block in 0..<count {
            let range = (block * 16)..<(block * 16 + 16)
            let a = sequentialSum(low, range), b = sequentialSum(full, range)
            powers.append(a); ratios.append(b > 0 ? a / b : 0)
        }
        let ratioMean: Double, powerMean: Double
        if beatCount < 128 {
            ratioMean = sum(ratios) / Double(count)
            powerMean = sum(powers) / Double(count)
        } else {
            let windows = beatCount / 64 - 1
            ratioMean = (0..<windows).map { sequentialSum(ratios, ($0 * 16)..<($0 * 16 + 16)) * 0.0625 }.max() ?? 0
            powerMean = (0..<windows).map { sequentialSum(powers, ($0 * 16)..<($0 * 16 + 16)) * 0.0625 }.max() ?? 0
        }
        return LowPower(ratio: ratio, blockRatios: ratios, blockPowers: powers,
                        maximumRatioMean: ratioMean, maximumPowerMean: powerMean)
    }

    /// Both the detected slot and its successor contribute; adjacent hits share
    /// the successor sample, while the denominator remains the number of hits.
    static func compression(low: [Double], full: [Double], rhythm: [UInt8], phase: Int) throws -> (first: Double, second: Double) {
        guard low.count == full.count, phase >= 1 else { throw InputError.inconsistentDimensions }
        let offset = (phase - 1) * 4
        let blocks = (rhythm.count - offset) / 128
        guard rhythm.count - offset >= 128 else { return (0.1, 0.3) }
        guard offset + blocks * 128 <= full.count else { throw InputError.unsupportedRange }
        var maximumFirst = 0.0, maximumSelectedLow = 0.0
        for block in 0..<blocks {
            var selectedLow = 0.0, selectedFull = 0.0
            var hits = 0, previousSuccessor = -1
            for slot in 0..<128 where rhythm[block * 128 + slot] == 1 {
                hits += 1
                let index = offset + block * 128 + slot
                if index != previousSuccessor, index < full.count {
                    selectedLow += low[index]; selectedFull += full[index]
                }
                if index + 1 < full.count {
                    selectedLow += low[index + 1]; selectedFull += full[index + 1]
                    previousSuccessor = index + 1
                } else { previousSuccessor = -1 }
            }
            guard hits >= 4 else { continue }
            let total = rangeSum(full, from: offset + block * 128, through: offset + block * 128 + 127)
            let off = hits < 128 ? (total - selectedFull) / Double(128 - hits) : 0
            let on = selectedFull / Double(hits)
            maximumFirst = max(maximumFirst, off + on > 0 ? off / (off + on) : 0)
            maximumSelectedLow = max(maximumSelectedLow, selectedLow / Double(hits))
        }
        return (maximumFirst, maximumSelectedLow * 0.0075175301)
    }

    static func slowRock(low: [UInt8], middle: [UInt8]) throws -> Int {
        guard middle.count >= low.count else { throw InputError.inconsistentDimensions }
        guard low.count > 32 else { return 0 }
        var previous = 0, matches = 0
        func both(_ index: Int, _ value: UInt8) -> Bool { low[index] == value && middle[index] == value }
        for start in 0..<(low.count - 32) {
            guard both(start, 1), (2...6).allSatisfy({ both(start + $0, 0) }),
                  (7...9).contains(where: { low[start + $0] == 0 && middle[start + $0] == 1 }),
                  (10...14).allSatisfy({ both(start + $0, 0) }),
                  (15...17).contains(where: { both(start + $0, 1) }) else { continue }
            if start == previous || start + 1 == previous || start + 2 == previous {
                matches += 1; previous = 0
            } else { previous = start + 17 }
        }
        return matches > 1 ? 1 : 0
    }

    struct RhythmFlags {
        var fourKick = 0
        var popRock = 0
        var alternateRock = 0
        var sparseKick = 0
    }

    static func rhythmFlags(rhythm: [UInt8], bpm: Double, blockPowers: [Double]) throws -> RhythmFlags {
        guard !blockPowers.isEmpty else { throw InputError.inconsistentDimensions }
        let mean = sum(blockPowers) / Double(blockPowers.count)
        var start = blockPowers.count < 17 ? 1 : 17
        if start <= blockPowers.count - 3 {
            for i in start...(blockPowers.count - 3) where blockPowers[i - 1] > mean { start = i; break }
        }
        let remaining = Int(Double(rhythm.count) * 0.063) - start
        let groups = min(8, remaining / 8)
        guard groups > 0 else { return RhythmFlags() }
        guard (start - 1 + groups * 8) * 16 <= rhythm.count else { throw InputError.unsupportedRange }
        let patterns: [[Int]] = [[0, 4, 8, 12], [2, 6, 10, 14], [0, 6, 8], [0, 8], [0, 8, 10]]
        var counts = [Int](repeating: 0, count: 4)
        for group in 0..<groups {
            var found = [Bool](repeating: false, count: 4)
            for offset in [1, 2, 5, 6] {
                let bar = start - 1 + group * 8 + offset
                if bar <= 0 { continue }
                let slice = rhythm[(bar * 16)..<(bar * 16 + 16)]
                let matches = patterns.map { pattern in
                    (0..<16).allSatisfy { slice[slice.startIndex + $0] == (pattern.contains($0) ? 1 : 0) }
                }
                found[0] = found[0] || matches[0] || matches[1]
                found[1] = found[1] || matches[2]
                found[2] = found[2] || matches[3]
                found[3] = found[3] || matches[4]
            }
            for i in counts.indices where found[i] { counts[i] += 1 }
        }
        let required = remaining > 63 ? 1 : 0
        var flags = RhythmFlags()
        if counts[0] >= 2 { flags.fourKick = 1 }
        else if counts[3] > required { flags.sparseKick = 1 }
        else if counts[1] > required && bpm < 165 { flags.popRock = 1 }
        else if counts[2] > required { flags.alternateRock = 1 }
        return flags
    }

    /// The native helper consumes sample-coordinate onset and beat indices.
    /// Its phase expression is retained as executed, including integer scaling.
    static func slowTempo(sampleRate: Double, bpm: Double, blockPowers: [Double], onsetSamples: [Int],
                          beatSamples: [Int], onsetCount: Int, firstOnset: Int,
                          sourceOffsetMS: Int, analysisOffsetMS: Int) throws -> (slow: Int, syncopated: Int) {
        guard firstOnset >= 1, onsetCount >= 0, firstOnset - 1 + onsetCount <= onsetSamples.count,
              blockPowers.count <= beatSamples.count / 4 else { throw InputError.inconsistentDimensions }
        var onset = onsetSamples.map(Double.init)
        let last = firstOnset + onsetCount
        let shift = sampleRate * 0.001 * Double(sourceOffsetMS - analysisOffsetMS)
        if onsetCount > 0 {
            for i in (firstOnset - 1)..<(last - 1) { onset[i] += shift }
        }
        let period = bpm == 0 ? sampleRate * 0.5 : sampleRate * 60 / bpm
        let roundedPeriod = Int(period + 0.5)
        guard roundedPeriod != 0 else { throw InputError.unsupportedRange }
        let quarter = Double(Int(period * 0.25 + 0.5))
        let powerMean = blockPowers.isEmpty ? 0 : sum(blockPowers) / Double(blockPowers.count)
        var votes = [Int]()
        if blockPowers.count > 3 && firstOnset <= last - 2 {
            for bar in 0..<(blockPowers.count - 3) where blockPowers[bar] > powerMean {
                let start = Double(beatSamples[bar * 4])
                let end = Double(beatSamples[min(bar * 4 + 31, beatSamples.count - 1)])
                var i = firstOnset - 1
                repeat {
                    let t = onset[i]
                    if t >= start && t <= end && i + 2 < last {
                        let divided = (t - start) * (1 / Double(roundedPeriod))
                        let position = divided - Double(Int(divided) * roundedPeriod)
                        let inside = position >= quarter && position <= quarter * 3
                        let gap = onset[i + 1] - t
                        if gap > period * 1.875 && gap < period * 2.125 && !inside && votes.count < 64 { votes.append(0) }
                        if gap > period * 0.875 && gap < period * 1.125 && inside && votes.count < 64 { votes.append(1) }
                    }
                    i += 1
                    if t > end { break }
                } while i < last - 2
                if votes.count >= 64 { break }
            }
        }
        let syncopated = !votes.isEmpty && Double(votes.reduce(0, +)) / Double(votes.count) > 0.5 ? 1 : 0
        return (syncopated == 0 && bpm < 83.9 ? 1 : 0, syncopated)
    }

    struct Recurrence {
        let scores: [[Double]]
        let threshold: Double
        let related: [Set<Int>]
    }

    /// Native ABmelo geometry: centered spectral rows, inverse squared distance,
    /// aligned common span, and positive-score mean plus sample deviation.
    static func recurrence(spectra: [[Double]], boundaries: [Int], phase: Int, endBeat: Int) throws -> Recurrence {
        guard let width = spectra.first?.count, width > 0,
              spectra.allSatisfy({ $0.count == width }), !boundaries.isEmpty else {
            throw InputError.inconsistentDimensions
        }
        let indices = (boundaries + [endBeat]).map { max(1, Int(Double($0 - phase) * 0.5 + 1)) }
        guard indices == indices.sorted() else { throw InputError.unsupportedRange }
        let n = boundaries.count
        let means = spectra.map { sum($0) / Double(width) }
        var scores = Array(repeating: Array(repeating: 0.0, count: n), count: n)
        // The native matrix has rowCount-1 rows; its last column is valid.
        for i in 0..<n {
            try Task.checkCancellation()
            for j in (i + 1)..<n {
                try Task.checkCancellation()
                let count = min(indices[i + 1] - indices[i], indices[j + 1] - indices[j], spectra.count - indices[j])
                if count <= 0 { continue }
                var total = 0.0
                for k in 0..<count {
                    let a = indices[i] + k, b = indices[j] + k
                    var lanes = [Double](repeating: 0, count: 8)
                    let vectorEnd = width & ~7
                    for c in 0..<vectorEnd {
                        let delta = (means[b] + spectra[a][c]) - (means[a] + spectra[b][c])
                        lanes[c & 7] = lanes[c & 7].addingProduct(delta, delta)
                    }
                    var distance = (lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))) + (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])))
                    for c in vectorEnd..<width {
                        let delta = (means[b] + spectra[a][c]) - (means[a] + spectra[b][c])
                        distance = distance.addingProduct(delta, delta)
                    }
                    total += distance == 0 ? 65_535 : 1 / distance
                }
                scores[i][j] = total / Double(count)
            }
        }
        let positives = scores.flatMap { $0 }.filter { $0 > 0 }
        let mean = positives.isEmpty ? 0 : sum(positives) / Double(positives.count)
        let squared = positives.map { ($0 - mean) * ($0 - mean) }
        let deviation = positives.count > 1 ? sqrt(sum(squared) / Double(positives.count - 1)) : sum(squared)
        let threshold = mean + deviation
        var related = Array(repeating: Set<Int>(), count: n)
        for i in 0..<n {
            for j in (i + 1)..<n where scores[i][j] >= threshold {
                related[i].insert(j); related[j].insert(i)
            }
        }
        return Recurrence(scores: scores, threshold: threshold, related: related)
    }

    struct SeedEvidence {
        let roles: [Int]
        let level: [Double]
        let middle: [Double]
        let vocal: [Double]
    }

    static func nonHighSeeds(middleLevels: [Double], openingEnvelope: [Double], boundaries: [Int], vocalActivity: [Double], mood: PhraseMood) throws -> SeedEvidence {
        guard !middleLevels.isEmpty, !boundaries.isEmpty, !vocalActivity.isEmpty else { throw InputError.inconsistentDimensions }
        var envelope = openingEnvelope
        if envelope.count < middleLevels.count * 4 { envelope += Array(repeating: 0, count: middleLevels.count * 4 - envelope.count) }
        let middle = normalized(middleLevels), vocal = normalized(vocalActivity)
        var relative = boundaries.map { ($0 - boundaries[0]) / 4 + 1 }.filter { $0 != 0 }.sorted()
        let count = relative.count
        guard count > 0 else { throw InputError.unsupportedRange }
        if sum(middle) == 0 {
            return SeedEvidence(roles: Array(repeating: 0, count: count), level: Array(repeating: 0, count: count), middle: Array(repeating: 0, count: count), vocal: Array(repeating: 0, count: count))
        }
        relative.append(middle.filter { $0 != 0 }.count - 2)
        guard relative.last! >= relative[count - 1] else { throw InputError.unsupportedRange }
        var levelMeans = [Double](), middleMeans = [Double](), vocalMeans = [Double]()
        for i in 0..<count {
            let start = relative[i], end = relative[i + 1]
            guard start >= 1, end >= 2, end - 2 < middle.count,
                  end * 4 - 5 < envelope.count, end * 4 - 5 < vocal.count else { throw InputError.unsupportedRange }
            func segment(_ values: [Double], start: Int, end: Int) -> Double {
                if end - start < 2 { return values[end - 1] }
                return rangeSum(values, from: start, through: end - 1) / Double(end - start)
            }
            levelMeans.append(segment(envelope, start: start * 4 - 4, end: end * 4 - 4))
            vocalMeans.append(segment(vocal, start: start * 4 - 4, end: end * 4 - 4))
            middleMeans.append(segment(middle, start: start - 1, end: end - 1))
        }
        levelMeans = normalized(levelMeans); middleMeans = normalized(middleMeans); vocalMeans = normalized(vocalMeans)
        let vocalNonzero = vocalMeans.filter { $0 != 0 }
        let vocalMean = vocalNonzero.isEmpty ? 0 : sum(vocalNonzero) / Double(vocalNonzero.count)
        let levelMean = sum(levelMeans) / Double(count)
        var roles = Array(repeating: 0, count: count)
        roles[0] = 10
        func eligible(_ i: Int) -> Bool {
            middleMeans[i] >= 0.4 && (vocalMean < 0.4 || vocalMeans[i] >= 0.4)
        }
        func strong(_ i: Int) -> Bool {
            (levelMean > 0.8 && levelMeans[i] > 0.92) || (levelMean <= 0.8 && levelMeans[i] > 0.91) || (levelMeans[i] > 0.8 && vocalMeans[i] > 0.88)
        }
        if eligible(0) && strong(0) { roles[0] = 40 }
        if count > 1 {
            for i in 1..<count where eligible(i) {
                if strong(i) { roles[i] = 40; continue }
                if vocalMeans[i] == 0 { vocalMeans[i] = 0.003 }
                if vocalMeans[i - 1] == 0 { vocalMeans[i - 1] = 0.003 }
                if levelMeans[i] > 0.85 && vocalMeans[i] > 0.65 && vocalMeans[i] / vocalMeans[i - 1] > 1.3 {
                    roles[i] = 40; continue
                }
                if levelMeans[i] == 0 { levelMeans[i] = 0.003 }
                if levelMeans[i - 1] == 0 { levelMeans[i - 1] = 0.003 }
                let ratio = levelMeans[i] / levelMeans[i - 1]
                if (levelMeans[i] > 0.85 && ratio > 1.5) || (roles[i - 1] == 40 && ratio > 0.88) { roles[i] = 40 }
            }
            for i in 1..<count {
                let decline = mood == .mid ? (roles[i - 1] == 40 ? 0.6 : 0.5) : (roles[i - 1] == 40 ? 0.62 : 0.54)
                if roles[i - 1] == 10 && vocalMeans[i] == 0 {
                    if roles[i] != 40 { roles[i] = 10 }
                } else if roles[i] != 40 && vocalMeans[i] < vocalMeans[i - 1] * decline { roles[i] = 50 }
            }
        }
        return SeedEvidence(roles: roles, level: levelMeans, middle: middleMeans, vocal: vocalMeans)
    }

    /// Keep the ordered upper-triangle pass: converting this relation into
    /// connected components changes conflict and residual-material allocation.
    static func reconcileNonHigh(roles seeds: [Int], related: [Set<Int>]) -> [Int] {
        precondition(seeds.count == related.count)
        guard !seeds.isEmpty else { return [] }
        var roles = seeds, removed = Array(repeating: false, count: seeds.count)
        let n = roles.count
        for i in 0..<n {
            for j in (i + 1)..<n where related[i].contains(j) {
                if roles[i] == 50 && roles[j] == 40 { roles[i] = 0; removed[i] = true }
                else if roles[i] == 40 && roles[j] == 50 { roles[j] = 0; removed[j] = true }
            }
        }
        var next = 61
        for i in 0..<n {
            var created = false
            for j in (i + 1)..<n where related[i].contains(j) {
                if roles[i] == 0 {
                    if roles[j] == 0 { roles[i] = next; roles[j] = next; created = true }
                    else if roles[j] == 40 { if !removed[i] { roles[i] = 40 } }
                    else if roles[j] != 10 && roles[j] != 50 { roles[i] = roles[j] }
                } else if roles[j] == 0 && roles[i] != 10 && roles[i] != 50 {
                    if roles[i] != 40 || !removed[j] { roles[j] = roles[i] }
                }
            }
            if created { next += 1 }
        }
        if roles[n - 1] != 40 { roles[n - 1] = 60 }
        if n > 2 {
            for i in 1..<(n - 1) where roles[i] == 0 { roles[i] = next; next += 1 }
        }
        return roles
    }

    private static func normalized(_ values: [Double]) -> [Double] {
        let maximum = values.max() ?? 0
        let multiplier = maximum == 0 ? 333.3333333333333 : 1 / maximum
        return values.map { $0 * multiplier }
    }

    private static func sequentialSum(_ values: [Double], _ range: Range<Int>) -> Double {
        range.reduce(0) { $0 + values[$1] }
    }

    private static func rangeSum(_ values: [Double], from start: Int, through end: Int) -> Double {
        guard end >= start else { return 0 }
        return sum(Array(values[start...end]))
    }

    /// Match the eight-lane reductions used by the native MatArray sums.
    private static func sum(_ values: [Double]) -> Double {
        var lanes = [Double](repeating: 0, count: 8)
        let vectorEnd = values.count & ~7
        for i in 0..<vectorEnd { lanes[i & 7] += values[i] }
        var total = (lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))) + (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])))
        for i in vectorEnd..<values.count { total += values[i] }
        return total
    }
}
