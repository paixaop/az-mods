#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseRhythmFeatures {
    struct Block {
        let index: Int
        let firstBeat: Int
        let endBeat: Int
        let phase: Int
        let offsetMilliseconds: Int
        let low: Range<Int>
        let middle: Range<Int>
        let high: Range<Int>
        var beatCount: Int { endBeat - firstBeat }
    }

    static func blocks(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                       coordinates: NativePhraseFeatures.Coordinates) throws -> [Block] {
        var result = [Block](), begin = 0
        let count = coordinates.beatCount, period = 60 / timing.bpm
        while begin < count {
            let phase = result.isEmpty ? timing.phase : 1
            let offset = result.isEmpty ? timing.offsetMilliseconds : 0
            let nominal = NativePhraseFeatures.nominalBlockBeatCount(period: period, phase: phase,
                offsetSeconds: Double(offset) * 0.001)
            guard nominal > 0 else { throw NativePhraseFeatures.InputError.invalidTiming }
            let end = min(count, begin + nominal)
            let partial = nominal > count - begin
            func range(_ positions: [Int], _ length: Int) -> Range<Int> {
                (begin == 0 ? 0 : positions[begin] - 1)..<(partial ? length : positions[end] - 1)
            }
            result.append(Block(index: result.count + 1, firstBeat: begin, endBeat: end,
                phase: phase, offsetMilliseconds: offset, low: range(coordinates.low, samples.low.count),
                middle: range(coordinates.middle, samples.middle.count), high: range(coordinates.high, samples.high.count)))
            begin = end
        }
        return result
    }

    struct Result {
        let lowPower: [Double]
        let subPower: [Double]
        let allPower: [Double]
        let allPeaks: [Double]
        let firstAttacks: [UInt8]
        let secondAttacks: [UInt8]
        let hiHatCounts: [UInt8]
        let hiHatChanges: [UInt8]
        let dividedLow: [Double]
        let dividedMiddle: [Double]
        let finalMiddleCoordinates: [Int]
        let zeroDataFlag: Int
    }

    static func make(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                     coordinates: NativePhraseFeatures.Coordinates) throws -> Result {
        let count = coordinates.beatCount
        let lowRate = samples.sourceRate / 32, middleRate = samples.sourceRate / 8, highRate = samples.sourceRate / 2
        var low = NativePhraseRhythm.PowerProcessor(rate: lowRate, low: true)
        var sub = SubPowerProcessor(rate: lowRate)
        var all = NativePhraseRhythm.PowerProcessor(rate: middleRate, low: false)
        var firstAttacks = NativePhraseRhythm.Attacks(rate: middleRate, second: false)
        var secondAttacks = NativePhraseRhythm.Attacks(rate: middleRate, second: true)
        var lowPower = [Double](repeating: 0, count: count * 4), subPower = lowPower, allPower = lowPower, allPeaks = lowPower
        let bars = max(0, (count - timing.phase + 1) / 4)
        var first = [UInt8](repeating: 0, count: bars * 16), second = first
        var hiHat = [UInt8](repeating: 0, count: count)
        var zeroDataFlag = 0
        let blocks = try blocks(samples: samples, timing: timing, coordinates: coordinates)
        for block in blocks {
            try Task.checkCancellation()
            let begin = block.firstBeat, end = block.endBeat, n = block.beatCount
            func region(_ source: [Double], _ positions: [Int], _ range: Range<Int>) -> (samples: [Double], coordinates: [Int]) {
                (Array(source[range]), positions[begin...end].map { $0 - range.lowerBound })
            }
            let l = region(samples.low, coordinates.low, block.low)
            let m = region(samples.middle, coordinates.middle, block.middle)
            let h = region(samples.high, coordinates.high, block.high)
            if zeroDataFlag == 0 && l.samples.contains(where: { $0 != 0 }) { zeroDataFlag = 1 }
            let lp = low.process(l.samples, rate: lowRate, bpm: timing.bpm, coordinates: l.coordinates, beatCount: n)
            let sp = sub.process(l.samples, rate: lowRate, bpm: timing.bpm, coordinates: l.coordinates, beatCount: n)
            let ap = all.process(m.samples, rate: middleRate, bpm: timing.bpm, coordinates: m.coordinates, beatCount: n)
            lowPower.replaceSubrange((begin * 4)..<(end * 4), with: lp.quarters)
            subPower.replaceSubrange((begin * 4)..<(end * 4), with: sp)
            allPower.replaceSubrange((begin * 4)..<(end * 4), with: ap.quarters)
            allPeaks.replaceSubrange((begin * 4)..<(end * 4), with: ap.peaks)
            let a = firstAttacks.process(m.samples, rate: middleRate, bpm: timing.bpm,
                offsetMilliseconds: block.offsetMilliseconds, phase: block.phase, block: block.index, coordinates: m.coordinates, beatCount: n)
            let b = secondAttacks.process(m.samples, rate: middleRate, bpm: timing.bpm,
                offsetMilliseconds: block.offsetMilliseconds, phase: block.phase, block: block.index, coordinates: m.coordinates, beatCount: n)
            let completedBars = (end - timing.phase + 1) / 4
            func place(_ values: [UInt8], in destination: inout [UInt8]) {
                let start = completedBars * 16 - values.count
                if start < 0 || completedBars < 1 { destination = [0] }
                else { destination.replaceSubrange(start..<(start + values.count), with: values) }
            }
            place(a, in: &first); place(b, in: &second)
            hiHat.replaceSubrange(begin..<end,
                with: hiHatCounts(samples: h.samples, sampleRate: highRate, bpm: timing.bpm, coordinates: h.coordinates, beatCount: n))
        }
        let dividedSamples = NativePhraseFeatures.Samples(sourceRate: samples.sourceRate,
            sourceFrameCount: samples.sourceFrameCount,
            low: Array(samples.low.prefix(blocks.last?.low.upperBound ?? 0)),
            middle: Array(samples.middle.prefix(blocks.last?.middle.upperBound ?? 0)), high: [])
        let divided = NativePhraseDecorators.dividedBands(samples: dividedSamples, coordinates: coordinates)
        return Result(lowPower: lowPower, subPower: subPower, allPower: allPower, allPeaks: allPeaks,
            firstAttacks: first, secondAttacks: second, hiHatCounts: hiHat,
            hiHatChanges: hiHatChanges(hiHat, phase: timing.phase), dividedLow: divided.low, dividedMiddle: divided.middle,
            finalMiddleCoordinates: retainedCoordinates(coordinates.middle, blocks: blocks), zeroDataFlag: zeroDataFlag)
    }

    /// The caller translates the entire coordinate storage, then shifts only its
    /// live prefix. The unused suffix remains in the exported array.
    static func retainedCoordinates(_ original: [Int], blocks: [Block]) -> [Int] {
        var result = original
        for block in blocks.dropLast() {
            let count = block.beatCount, origin = result[count]
            for i in result.indices { result[i] -= origin }
            for i in 0..<(result.count - count) { result[i] = result[i + count] + 1 }
        }
        return result
    }

    struct SubPowerProcessor {
        var sections: [NativePhraseEnvelopes.Section]
        var reduction: NativePhraseRhythm.PowerProcessor

        init(rate: Double) {
            sections = Array(repeating: NativePhraseEnvelopes.secondOrder(rate: rate,
                angularCutoff: 251.32741228718345, damping: 1.4142135623730951, highPass: false), count: 4)
            reduction = NativePhraseRhythm.PowerProcessor(rate: rate, low: false)
        }

        mutating func process(_ input: [Double], rate: Double, bpm: Double,
                              coordinates: [Int], beatCount: Int) -> [Double] {
            var filtered = input
            for i in sections.indices { filtered = sections[i].process(filtered) }
            let powers = reduction.process(filtered, rate: rate, bpm: bpm, coordinates: coordinates, beatCount: beatCount).quarters
            return rate > 5512.45 && rate < 5512.55 ? powers : powers.map { $0 * 4 }
        }
    }

    /// The two followers begin from their initialization values on each call;
    /// the native routine does not persist the final followers back to state.
    static func hiHatCounts(samples: [Double], sampleRate: Double, bpm: Double,
                            coordinates: [Int], beatCount: Int, fast: Double = 0, slow: Double = 0) -> [UInt8] {
        var fast = fast, slow = slow, previousPosition = 0
        var result = [UInt8](repeating: 0, count: beatCount)
        let reciprocalRate = 1 / sampleRate
        let minimumGap = sampleRate * 13.333333333333332 / bpm
        for (i, sample) in samples.enumerated() {
            let value = abs(sample)
            fast = fast.addingProduct((value < fast ? 20 : 2000) * (value - fast), reciprocalRate)
            slow = slow.addingProduct((value < slow ? 13.333333333333334 : 1000) * (value - slow), reciprocalRate)
            if slow < fast && fast > 0.06 && minimumGap < Double(i + 1 - previousPosition) {
                previousPosition = i + 1
                for beat in 0..<beatCount where coordinates[beat] <= i + 1
                    && (beat == beatCount - 1 || i + 1 < coordinates[beat + 1]) {
                    result[beat] &+= 1
                }
            }
        }
        return result
    }

    static func hiHatChanges(_ counts: [UInt8], phase: Int) -> [UInt8] {
        let remaining = counts.count - phase + 1
        guard remaining > 0 else { return [] }
        let bars = (remaining + 3) / 4
        var totals = [UInt8](repeating: 0, count: bars)
        for i in 0..<remaining { totals[i / 4] &+= counts[phase - 1 + i] }
        var levels = totals.map { $0 < 3 ? UInt8(0) : $0 < 7 ? 4 : $0 < 12 ? 8 : 16 }
        for length in 1...3 where bars >= length + 2 {
            var changed = true
            while changed {
                changed = false
                for i in 1..<(bars - length) {
                    let level = levels[i - 1]
                    if level == levels[i + length] && (i..<(i + length)).allSatisfy({ levels[$0] != level }) {
                        for j in i..<(i + length) { levels[j] = level }
                        changed = true
                    }
                }
            }
        }
        var changes = [UInt8](repeating: 0, count: bars)
        if bars > 1 {
            for i in 1..<bars where levels[i] != levels[i - 1] { changes[i] = 1 }
        }
        return changes
    }
}
