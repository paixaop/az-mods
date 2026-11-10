#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Beat-aligned power and the two causal sixteenth-note attack streams.
enum NativePhraseRhythm {
    struct Power {
        let quarters: [Double]
        let peaks: [Double]
        let bars: [Double]
    }

    struct PowerProcessor {
        var filter: NativePhraseEnvelopes.Section?
        let low: Bool

        init(rate: Double, low: Bool, cutoff: Double = 100) {
            self.low = low
            filter = low ? NativePhraseEnvelopes.secondOrder(rate: rate,
                angularCutoff: cutoff * 6.283185307179586, damping: 1.4142135623730951,
                highPass: false) : nil
        }

        mutating func process(_ input: [Double], rate: Double, bpm: Double,
                              coordinates: [Int], beatCount: Int) -> Power {
            let source: [Double]
            if filter != nil { source = filter!.process(input) } else { source = input }
            let period = rate * 60 / bpm
            let quarter = Int(period * 0.25 + 0.5)
            var powers = [Double](repeating: 0, count: beatCount * 4)
            var peaks = powers
            for beat in 0..<beatCount {
                let start = coordinates[beat]
                let end = beat + (low ? 2 : 1) < beatCount
                    ? coordinates[beat + 1] : start + Int(period + 0.5) - 1
                for part in 0..<4 {
                    let first = start - 1 + quarter * part
                    let last = part == 3 ? end - 1 : first + quarter
                    guard first >= 0, last >= first, last <= source.count else { continue }
                    powers[beat * 4 + part] = squareSum(source, first..<last)
                    peaks[beat * 4 + part] = source[first..<last].map(abs).max() ?? 0
                }
            }
            if low && !(rate > 5512.45 && rate < 5512.55) {
                powers = powers.map { $0 * 4 }
            }
            let bars = (0..<(beatCount / 4)).map { bar in
                powers[(bar * 16)..<(bar * 16 + 16)].reduce(0, +)
            }
            return Power(quarters: powers, peaks: peaks, bars: bars)
        }
    }

    struct Attacks {
        var lowPass: [NativePhraseEnvelopes.Section]
        var highPass: [NativePhraseEnvelopes.Section]
        var delaySmoother: NativePhraseEnvelopes.Section
        var bandSmoother: NativePhraseEnvelopes.Section
        let second: Bool
        var previousEnvelope: Double
        var delay: Int
        var maximumAttack: Double

        init(rate: Double, second: Bool, delay: Int = 0,
             previousEnvelope: Double = 0, maximumAttack: Double = 0.0001) {
            let damping = [1.8477590650225735, 0.7653668647301797,
                           1.8477590650225735, 0.7653668647301797]
            let full = [5512.5, 6000, 4000].contains { abs(rate - $0) < 0.01 }
            let reduced = [1378.125, 1500, 1000].contains { abs(rate - $0) < 0.01 }
            precondition(full || reduced, "Unsupported native rhythm sample rate")
            lowPass = (full ? damping : [1.4142135623730951]).map {
                NativePhraseEnvelopes.secondOrder(rate: rate, angularCutoff: 1256.6370614359173,
                    damping: $0, highPass: false)
            }
            highPass = damping.map {
                NativePhraseEnvelopes.secondOrder(rate: rate, angularCutoff: 691.1503837897545,
                    damping: $0, highPass: true)
            }
            delaySmoother = NativePhraseEnvelopes.secondOrder(rate: rate,
                angularCutoff: 62.83185307179586, damping: 1.4142135623730951, highPass: false)
            bandSmoother = delaySmoother
            self.second = second
            self.delay = delay
            self.previousEnvelope = previousEnvelope
            self.maximumAttack = maximumAttack
        }

        mutating func process(_ input: [Double], rate: Double, bpm: Double,
                              offsetMilliseconds: Int, phase: Int, block: Int,
                              coordinates: [Int], beatCount: Int,
                              integerPeriod: Bool = false) -> [UInt8] {
            guard !input.isEmpty, beatCount > 0 else { return [0] }
            let period = integerPeriod ? Double(Int(rate * 60 / bpm + 0.5)) : rate * 60 / bpm
            let start = Int(Double(Int(rate * Double(offsetMilliseconds) * 0.001 + 0.5) + 1)
                + Double(phase - 1) * period)
            let barLength = Int(period * 4)
            var bars = (beatCount - phase + 1) / 4
            if input.count < start - 1 + bars * barLength { bars -= 1 }
            var filtered = input
            for i in lowPass.indices { filtered = lowPass[i].process(filtered) }
            let lowEnvelope = delaySmoother.process(filtered.map(abs))
            if block < 2 { maximumAttack = 0.0001 }
            guard bars > 0 else {
                previousEnvelope = lowEnvelope.last ?? 0
                return [0]
            }
            let oldDelay = delay
            delay = Self.findDelay(lowEnvelope, start: start, period: period,
                barCount: bars, ascendingOnly: second, roundedQuarter: integerPeriod)
            let activeDelay = block < 2 ? delay : oldDelay
            if integerPeriod && second { delay = oldDelay }
            var envelope = lowEnvelope
            if second {
                for i in highPass.indices { filtered = highPass[i].process(filtered) }
                envelope = bandSmoother.process(filtered.map(abs))
            }
            let previous = previousEnvelope
            if !integerPeriod || second { previousEnvelope = envelope.last ?? 0 }
            for i in stride(from: envelope.count - 1, through: 1, by: -1) {
                envelope[i] -= envelope[i - 1]
            }
            envelope[0] -= previous
            var end = coordinates[beatCount - 1]
            let proposedEnd = end + Int(period) - 1
            if proposedEnd <= input.count { end = proposedEnd }
            if activeDelay < start && end - activeDelay <= envelope.count {
                envelope = Array(envelope[(start - 1 - activeDelay)..<(end - activeDelay)])
            } else if start < activeDelay && end - activeDelay - 1 <= envelope.count {
                let prefixCount = max(0, end - activeDelay - 1)
                let source = Array(envelope.prefix(prefixCount))
                let zeroCount = activeDelay - (start - 1)
                let outputCount = max(envelope.count, end - start)
                envelope = [Double](repeating: 0, count: zeroCount) + source
                if envelope.count < outputCount {
                    envelope += [Double](repeating: 0, count: outputCount - envelope.count)
                }
            }
            var output = [UInt8](repeating: 0, count: bars * 16)
            if integerPeriod { maximumAttack = 0.0001 }
            let quarter = Int(period * 0.25 + (integerPeriod ? 0.5 : 0))
            for bar in 0..<bars {
                var peaks = [Double](repeating: 0, count: 16)
                for beat in 0..<4 {
                    let ordinal = phase - 1 + bar * 4 + beat
                    guard ordinal < coordinates.count else { return output }
                    let base = integerPeriod ? Double(bar * 4 + beat) * period
                        : Double(coordinates[ordinal] - start) + 0.5
                    for part in 0..<4 {
                        let first = Int(base + period * (Double(part) * 0.25))
                        guard first >= 0, first + quarter <= envelope.count else { return output }
                        peaks[beat * 4 + part] = envelope[first..<(first + quarter)].max() ?? 0
                    }
                }
                let barMaximum = peaks.max() ?? 0
                for _ in 0..<(second ? 12 : 6) {
                    let thresholdPeak = peaks.max() ?? 0
                    if thresholdPeak > 0.0001,
                       thresholdPeak > barMaximum * (second ? 0.5 : 0.596),
                       thresholdPeak > maximumAttack * 0.178 {
                        var index = 0
                        for i in 1..<16 where peaks[i] > peaks[index] { index = i }
                        output[bar * 16 + index] = 1
                        peaks[index] = 0
                    }
                }
                maximumAttack = max(maximumAttack, barMaximum)
            }
            return output
        }

        mutating func processTiming(_ input: [Double], rate: Double, bpm: Double,
                                    offsetMilliseconds: Int, phase: Int = 1, block: Int = 1) -> [UInt8] {
            let period = Int(rate * 60 / bpm + 0.5)
            guard period > 0 else { return [0] }
            let offset = Int(rate * Double(offsetMilliseconds) * 0.001 + 0.5)
            let count = max(0, (input.count - offset - (phase - 1) * period) / period + phase - 1)
            let coordinates = (0..<count).map { offset + 1 + $0 * period }
            return process(input, rate: rate, bpm: bpm, offsetMilliseconds: offsetMilliseconds,
                phase: phase, block: block, coordinates: coordinates, beatCount: count, integerPeriod: true)
        }

        /// The first mode also searches falling edges. Both retain a candidate
        /// peak between the eight passes, even when a later pass finds no peak.
        static func findDelay(_ envelope: [Double], start: Int, period: Double,
                              barCount: Int, ascendingOnly: Bool, roundedQuarter: Bool = false) -> Int {
            guard barCount > 1 else { return 0 }
            let barLength = Int(period * 4), quarter = Int(period * 0.25 + (roundedQuarter ? 0.5 : 0))
            let base = start - 1, regionLength = (barCount - 1) * barLength
            guard base + barLength >= 0, base + barLength + regionLength <= envelope.count else { return 0 }
            var search = Array(envelope[(base + barLength)..<(base + barLength + regionLength)])
            var peakPosition = 0, valleyPosition = 0
            var peak = 0.0
            for _ in 0..<8 {
                var maximumIndex = 0
                for i in search.indices where search[i] > search[maximumIndex] { maximumIndex = i }
                let maximum = search[maximumIndex]
                search[maximumIndex] = 0
                let bar = Int(Double(maximumIndex + 1) * 0.25 * (1 / period))
                let boundary = barLength + bar * barLength
                let position = base + boundary
                guard position - 1 >= 0, position + barLength <= envelope.count,
                      boundary < regionLength - barLength,
                      envelope[position - 1] > maximum * 0.2 else { continue }
                let rising = envelope[position - 1] <= envelope[position]
                    && envelope[position] <= envelope[position + 1]
                    && envelope[position + 1] <= envelope[position + 2]
                let falling = envelope[position - 1] > envelope[position]
                    && envelope[position] > envelope[position + 1]
                    && envelope[position + 1] > envelope[position + 2]
                if rising {
                    for step in 0..<quarter where position + step + 1 < envelope.count {
                        if envelope[position + step + 1] - envelope[position + step] < 0 {
                            peakPosition = boundary + step + 1
                            peak = envelope[base - 1 + peakPosition]
                            break
                        }
                    }
                } else if falling && !ascendingOnly {
                    for step in 0..<quarter where position - 3 - step >= 0 {
                        if envelope[position - 3 - step] - envelope[position - 2 - step] < 0 {
                            peakPosition = boundary - step - 1
                            peak = envelope[base - 1 + peakPosition]
                            break
                        }
                    }
                } else { continue }
                guard peak > maximum * 0.595662 else { continue }
                let minimum = envelope[(position - 1)..<(position + barLength)].min() ?? 0
                let threshold = (minimum + maximum) * 0.5
                var found = false, length = 0
                var valley = maximum
                for step in 1...max(1, Int(period * 1.25)) {
                    let relative = peakPosition - step
                    let index = start + relative - 2
                    guard index > 0, index < envelope.count else { break }
                    if found {
                        if envelope[index] <= valley * 1.12 {
                            length += 1
                            if length >= Int(period * 0.0625) {
                                if valley == 0 { return 0 }
                                if peak / valley > 2.11 {
                                    return min(start, Int(Double(boundary) + 0.5 - Double(valleyPosition)))
                                }
                                break
                            }
                        } else { found = false; length = 0 }
                    } else if envelope[index] <= envelope[index - 1] && envelope[index] < threshold {
                        found = true
                        valley = envelope[index]
                        valleyPosition = relative
                    }
                }
            }
            return 0
        }
    }

    static func squareSum(_ source: [Double], _ range: Range<Int>) -> Double {
        var value = 0.0
        for index in range { value += source[index] * source[index] }
        return value
    }
}
