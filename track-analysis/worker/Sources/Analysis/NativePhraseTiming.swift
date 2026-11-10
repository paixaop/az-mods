#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// The phrase analyzer's private grid correction; it does not replace the public tempo estimate.
enum NativePhraseTiming {
    private static func offsetEqual(_ a: Int, _ b: Int, rate: Double) -> Bool {
        guard rate.isFinite, rate > 0, rate <= Double(UInt64.max) else { return false }
        let order = compare_timestamps(Double(a), Double(b), 1000, rate, 1)
        return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL
    }
    struct Correction: Equatable {
        let offset: Int
        let type: Int
        let rhythmType: Int
        let strongAlignedBeat: Int
    }

    static func quarterPower(_ samples: [Double], rate: Double, bpm: Double,
                             offset: Int, cutoff: Double, coordinates: [Int]? = nil) -> [Double] {
        var filter = NativePhraseEnvelopes.secondOrder(rate: rate,
            angularCutoff: cutoff * 6.283185307179586, damping: 1.4142135623730951, highPass: false)
        let filtered = filter.process(samples)
        let period = rate * 60 / bpm, rounded = Int(period + 0.5)
        let quarter = Int(period * 0.25 + 0.5), first = Int(rate * Double(offset) * 0.001 + 0.5)
        guard rounded > 0, quarter > 0 else { return [] }
        let positions = coordinates ?? (0..<max(0, (samples.count - first) / rounded)).map { first + 1 + $0 * rounded }
        var output = [Double](repeating: 0, count: positions.count * 4)
        for i in positions.indices {
            let start = positions[i] - 1
            let end = coordinates == nil ? start + rounded
                : (i + 1 < positions.count ? positions[i + 1] - 1 : start + rounded - 1)
            for part in 0..<4 {
                let begin = start + part * quarter, stop = part == 3 ? end : begin + quarter
                if begin >= 0 && stop >= begin && stop <= filtered.count {
                    output[i * 4 + part] = NativePhraseRhythm.squareSum(filtered, begin..<stop) * 4
                }
            }
        }
        return output
    }

    static func correctOffset(samples: [Double], rate: Double, bpm: Double, offset: Int,
                              rhythm: [UInt8], beatCount: Int, snares: [Int],
                              snareCount: Int, firstSnare: Int) -> Correction {
        guard beatCount >= 16, rhythm.count / 32 >= 2 else {
            return Correction(offset: offset, type: 0, rhythmType: 0, strongAlignedBeat: 0)
        }
        let groups = beatCount / 8
        let firstPattern: [UInt8] = [1, 0, 0, 0, 0, 0, 1]
        let longPattern: [UInt8] = [1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0]
        func at(_ i: Int) -> UInt8 { i < rhythm.count ? rhythm[i] : 0 }
        func matches(_ i: Int, _ p: [UInt8]) -> Bool { p.indices.allSatisfy { at(i + $0) == p[$0] } }
        var a = [Int](), b = [Int](), c = [Int](), d = [Int]()
        for i in 0..<(groups * 16) {
            if matches(i, firstPattern) {
                if (7..<16).allSatisfy({ at(i + $0) == 0 }) { a.append(i) }
                if at(i + 8) == 0 && at(i + 9) == 0 { b.append(i) }
            }
            if matches(i, longPattern) { c.append(i) }
            if at(i) == 1 && at(i + 1) == 0 && (at(i + 3) == 1 || at(i + 7) == 1)
                && (at(i + 6) == 1 || at(i + 10) == 1) { d.append(i) }
        }
        var candidates = [Int](), threshold = 0.5, halfBeat = false
        if c.count >= 5 { candidates = c; threshold = 0.2 }
        else if a.count >= 5 { candidates = a; threshold = 0.4 }
        else {
            if d.count >= 17 {
                let residues = d.map { ($0 + 1) % 4 }, maximum = residues.max() ?? 0
                let required = Double(Float(d.count > 60 ? 0.3 : d.count > 25 ? 0.41 : 0.65))
                halfBeat = maximum == 3 && Double(residues.filter { $0 == maximum }.count) / Double(d.count) > required
            }
            if !halfBeat && b.count >= 5 { candidates = b; threshold = 0.45 }
        }
        let quarterMS = Int(60 / bpm * 250)
        var reducedOffset = offset
        if quarterMS > 0 { while reducedOffset > quarterMS { reducedOffset -= quarterMS } }
        let onset = quarterPower(samples, rate: rate, bpm: bpm, offset: reducedOffset, cutoff: 400)
        let low = quarterPower(samples, rate: rate, bpm: bpm, offset: offset, cutoff: 100)
        let onsetMaximum = onset.max() ?? 0
        var onsetIndex = 0, priorSilence = false
        for i in onset.indices where onset[i] > onsetMaximum * 0.48 {
            if i > 0 && abs(onset[i - 1] - onset[i]) <= onset[i] * 0.6000000000000001 { continue }
            onsetIndex = i
            priorSilence = i > 0 && onset[..<i].allSatisfy { $0 == 0 }
            break
        }
        let onsetOffset = (onsetIndex <= 3 || priorSilence) && !onset.isEmpty && onset[onsetIndex] > 70
        let maximum = low.max() ?? 0
        var strong = false, first = 0
        if let index = low.firstIndex(where: { $0 > maximum * 0.5 }) {
            first = index
            var run = 0
            for i in stride(from: first, to: low.count, by: 4) {
                if low[i] > maximum * 0.605 {
                    if run > 14 { strong = true; break }
                    run += 1
                } else { run = 0 }
            }
            if !strong {
                var good = 0
                for start in stride(from: first, to: low.count, by: 64) {
                    for i in stride(from: start, to: min(start + 64, low.count), by: 4) {
                        if low[i] > maximum * 0.605 {
                            let previous = run; run += 1
                            if previous > 10 { good += 1; break }
                        } else { run = 0 }
                    }
                    if good > 2 { strong = true; break }
                    if run <= 11 { good = 0 }
                    run = 0
                }
            }
        }
        var snareHalf = false
        if !candidates.isEmpty && snareCount > 0 && firstSnare >= 1 && firstSnare <= groups * 8 {
            let period = 60 / bpm * rate
            var selected = Set<Int>()
            for candidate in candidates {
                for i in (firstSnare - 1)..<min(groups * 8, snares.count)
                    where Double(snares[i]) > period * 0.25 * Double(candidate) { selected.insert(snares[i]) }
            }
            let rounded = Int(period + 0.5)
            let count = selected.filter {
                let remainder = Double($0 % rounded)
                return period * 0.25 < remainder && remainder < period * 0.75
            }.count
            snareHalf = !selected.isEmpty && Double(count) / Double(selected.count) > threshold
        }
        let finalOffset: Int, type: Int
        if onsetOffset {
            finalOffset = reducedOffset + onsetIndex * quarterMS
            type = offsetEqual(finalOffset, offset, rate: rate) ? 3 : 2
        } else if halfBeat || snareHalf || (strong && (first + 1) % 4 == 3) {
            let shifted = offset - Int(60 / bpm * 500 + 0.5)
            finalOffset = shifted < 0 ? shifted + Int(60 / bpm * 1000 + 0.5) : shifted
            type = 1
        } else { finalOffset = offset; type = 0 }
        return Correction(offset: finalOffset, type: type, rhythmType: a.isEmpty ? 0 : 1,
                          strongAlignedBeat: strong && offsetEqual(finalOffset, offset, rate: rate) ? 1 : 0)
    }

    struct BarCorrection: Equatable {
        let phase: Int
        let parity: Int
        let ones: Int
        let valid: Int
        let ratio: Double
        let shifted: Int
    }

    static func correctBar(samples: [Double], rate: Double, bpm: Double, originalOffset: Int,
                           correctedOffset: Int, initialPhase: Int, coordinates: [Int],
                           snares: [Int], strengths: [Double], count: Int, first: Int,
                           suppliedPhase: Int, correctionType: Int) -> BarCorrection {
        let period = Int(rate * 60 / bpm + 0.5), quarter = Int(Double(period) * 0.25 + 0.5)
        let offset = rate * 0.001 * Double(originalOffset - correctedOffset)
        var ones = 0, valid = 0, odd = 0.0, even = 0.0
        if count > 0 && first > 0 {
            for i in (first - 1)..<min(first - 1 + count, snares.count, strengths.count) {
                let remainder = Int(Double(snares[i]) + offset + 0.5) % (period * 2)
                if remainder <= quarter || remainder > period * 2 - quarter {
                    ones += 1; valid += 1; odd += abs(strengths[i])
                } else if period - quarter <= remainder && remainder <= period + quarter {
                    valid += 1; even += abs(strengths[i])
                }
            }
        }
        let ratio = valid > 0 ? Double(ones) / Double(valid) : 1
        let parity = valid < 7 ? (suppliedPhase - 1) % 2 : ratio > 0.886 ? 1 : ratio <= 0.15 ? 0 : odd > even ? 1 : 0
        let power = quarterPower(samples, rate: rate, bpm: bpm, offset: correctedOffset, cutoff: 100, coordinates: coordinates)
        let threshold = (power.reduce(0, +) * 0.6) / Double(max(1, power.count)) + (power.max() ?? 0) * 0.4
        var shift = 0
        if power.count > 4 {
            for i in 0..<(power.count - 4) where power[parity * 4 + i] > threshold {
                let slot = i & 15
                if slot <= 2 || slot >= 13 { break }
                if (5...8).contains(slot) { shift = 2; break }
            }
        }
        let phase: Int
        if correctionType == 1 || (suppliedPhase + 1) % 2 != parity { phase = initialPhase + shift + (parity == 1 ? 1 : 0) }
        else { phase = correctionType & ~1 != 2 ? suppliedPhase : initialPhase }
        return BarCorrection(phase: phase, parity: parity, ones: ones, valid: valid, ratio: ratio, shifted: shift == 2 ? 1 : 0)
    }
}
