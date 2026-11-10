#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseExport {
    struct Variation {
        let flag: Int
        let summary: Double
    }

    struct Result {
        let mood: PhraseMood
        let endBeat: Int
        let entries: [ANLZPhraseEntry]
        let variation: Variation
        let converterFlag: Int
        let auxiliary: [Int]

        func phrases(confidence: Double, omitCollapsedIntervals: Bool = false, sampleRate: Double,
                     timeAtBeat: (Int) -> Double?) throws -> [TrackPhrase] {
            guard sampleRate.isFinite, sampleRate > 0, sampleRate < Double(UInt64.max) else {
                throw NativePhraseClassifier.InputError.unsupportedRange
            }
            return try entries.indices.compactMap { i -> TrackPhrase? in
                let entry = entries[i], followingBeat = i + 1 < entries.count ? entries[i + 1].startBeat : endBeat
                guard let start = timeAtBeat(entry.startBeat), let end = timeAtBeat(followingBeat) else {
                    throw NativePhraseClassifier.InputError.unsupportedRange
                }
                // Detailed-grid endpoint clamping can collapse a terminal
                // region. The native renderer omits its zero-width span.
                let order = compare_timestamps(start, end, 1, sampleRate, 1)
                guard order != AZ_TIME_INVALID else { throw NativePhraseClassifier.InputError.unsupportedRange }
                if omitCollapsedIntervals && order == AZ_TIME_EQUAL { return nil }
                let label = entry.code.label(in: mood)
                var fill: PhraseFillIn?
                let length = followingBeat - entry.fillStartBeat
                if entry.fill != 0, (1...4).contains(length), label.kind.supportsFillIn,
                   entry.fillStartBeat >= entry.startBeat, let fillStart = timeAtBeat(entry.fillStartBeat),
                   !omitCollapsedIntervals || fillStart < end {
                    fill = PhraseFillIn(start: fillStart, end: end, beatCount: Double(length), confidence: confidence)
                }
                return try TrackPhrase(start: start, end: end, label: label, confidence: confidence,
                    source: .inferredAudio, fillIn: fill, referenceCode: entry.code)
            }
        }
    }

    /// Stage D restores the recurrence-derived roles after calculating the
    /// terminal with temporary High roles. The temporary role changes must not
    /// replace the final non-High labels.
    static func nonHigh(mood: PhraseMood, boundaries: [Int], recurrenceSpectra: [[Double]], phase: Int, endBeat: Int,
                        middleLevels: [Double], openingEnvelope: [Double], vocalActivity: [Double],
                        lowPeaks: [Double], activity: [UInt8], lowQuarters: [Double], powerQuarters: [Double],
                        converterParameter: Int) throws -> Result {
        guard mood != .high, !boundaries.isEmpty else { throw NativePhraseClassifier.InputError.inconsistentDimensions }
        let n = boundaries.count, zeros = [Int](repeating: 0, count: n)
        var roles: [Int], temporaryRoles: [Int], metric: [Double]
        let variation: Variation
        if n == 1 {
            roles = [10]; temporaryRoles = roles; metric = [0]
            variation = Variation(flag: 0, summary: 0)
        } else {
            let recurrent = try NativePhraseClassifier.recurrence(spectra: recurrenceSpectra, boundaries: boundaries, phase: phase, endBeat: endBeat)
            let seeds = try NativePhraseClassifier.nonHighSeeds(middleLevels: middleLevels, openingEnvelope: openingEnvelope,
                boundaries: boundaries, vocalActivity: vocalActivity, mood: mood)
            roles = NativePhraseClassifier.reconcileNonHigh(roles: seeds.roles, related: recurrent.related)
            let blocks = NativePhraseHighClassifier.blockMeans(boundaries: boundaries, intro: openingEnvelope,
                low: lowPeaks, quarterPower: powerQuarters, phase: phase)
            temporaryRoles = NativePhraseHighClassifier.classify(boundaries: boundaries, introMeans: blocks.intro, lowMeans: blocks.low).roles
            metric = blocks.low
            variation = Self.variation(envelope: openingEnvelope, boundaries: boundaries)
        }
        let terminal = NativePhraseDecorators.outro(boundaries: boundaries, roles: temporaryRoles, low: lowPeaks,
            activity: activity, quarters: lowQuarters, phase: phase).terminal
        var fills = NativePhraseFills.Result(roles: roles, starts: zeros, auxiliary: zeros, routes: [NativePhraseFills.Route?](repeating: nil, count: n))
        let ends = boundaries + [terminal]
        NativePhraseFills.finalAdjust(boundaries: ends, highMood: false, phraseMetric: metric, result: &fills)
        let decorated = NativePhraseDecorators.Decorated(boundaries: ends, fills: fills, upStarts: zeros,
            upFirst: zeros, upSecond: zeros, upThird: zeros)
        return convert(mood: mood, decorated: decorated, variation: variation, converterParameter: converterParameter)
    }

    /// The native variation summary excludes the final span and includes both
    /// boundary samples in each preceding mean. Phase is not read by this stage.
    static func variation(envelope: [Double], boundaries: [Int]) -> Variation {
        guard boundaries.count >= 3 else { return Variation(flag: 0, summary: 0) }
        var limits = boundaries
        limits[limits.count - 1] = min(limits.last!, envelope.count)
        let means = (0..<(limits.count - 1)).map { i -> Double in
            let start = limits[i] - 1, end = limits[i + 1] - 1
            guard end >= start else { return 0 }
            return sum(envelope, start: start, end: end) / Double(end - start + 1)
        }
        var reference = means.count > 2 ? sum(means, start: 1, end: means.count - 2) / Double(means.count - 2) : 0
        let omitFirstInterior = means[1] < reference * 0.5 && boundaries.count > 3
        if omitFirstInterior { reference = sum(means, start: 2, end: boundaries.count - 2) / Double(boundaries.count - 3) }
        let first = omitFirstInterior ? 2 : 1
        let threshold = omitFirstInterior ? 0.075 : 0.09
        var deviations = [Double]()
        if first < means.count - 1 {
            for i in first..<(means.count - 1) where limits[i + 1] - limits[i] > 16 || means[i] > reference * 0.7 {
                deviations.append(abs(reference - means[i]))
            }
        }
        let summary = deviations.isEmpty ? 0 : sum(deviations, start: 0, end: deviations.count - 1) / Double(deviations.count)
        return Variation(flag: summary >= threshold ? 1 : 0, summary: summary)
    }

    static func convert(mood: PhraseMood, decorated: NativePhraseDecorators.Decorated,
                        variation: Variation, converterParameter: Int) -> Result {
        let n = decorated.fills.roles.count
        precondition(decorated.boundaries.count == n + 1)
        let converted = fields(mood: mood, roles: decorated.fills.roles, auxiliary: decorated.fills.auxiliary,
            upFirst: decorated.upFirst, upSecond: decorated.upSecond, upThird: decorated.upThird, upStarts: decorated.upStarts)
        let entries = (0..<n).map { i in
            ANLZPhraseEntry(index: i + 1, startBeat: decorated.boundaries[i],
                code: RekordboxPhraseCode(kind: converted.kind[i], k1: converted.k1[i], k2: converted.k2[i], k3: converted.k3[i]),
                b: converted.b[i], beat2: converted.beat2[i], beat3: converted.beat3[i], beat4: converted.beat4[i],
                fill: converted.fill[i], fillStartBeat: decorated.fills.starts[i])
        }
        return Result(mood: mood, endBeat: decorated.boundaries.last!, entries: entries, variation: variation,
            converterFlag: converterParameter < 12 ? 1 : 0, auxiliary: converted.auxiliary)
    }

    struct Fields {
        var kind: [Int], k1: [Int], k2: [Int], b: [Int], beat2: [Int], beat3: [Int], beat4: [Int], k3: [Int], fill: [Int], auxiliary: [Int]
    }

    static func fields(mood: PhraseMood, roles: [Int], auxiliary: [Int], upFirst: [Int], upSecond: [Int], upThird: [Int], upStarts: [Int]) -> Fields {
        let n = roles.count
        precondition([auxiliary, upFirst, upSecond, upThird, upStarts].allSatisfy { $0.count == n })
        let zeros = [Int](repeating: 0, count: n)
        var output = Fields(kind: zeros, k1: zeros, k2: zeros, b: zeros, beat2: zeros, beat3: zeros, beat4: zeros, k3: zeros, fill: zeros, auxiliary: auxiliary)
        if mood != .high {
            var materialKinds = [Int: Int](), next = 1
            let cycle = [7, 2, 3, 4, 5, 6]
            for i in roles.indices {
                switch roles[i] {
                case 10: output.kind[i] = 1
                case 40: output.kind[i] = 9
                case 50: output.kind[i] = 8
                case 60: output.kind[i] = 10
                case 61...:
                    if materialKinds[roles[i]] == nil { materialKinds[roles[i]] = cycle[next % 6]; next += 1 }
                    output.kind[i] = materialKinds[roles[i]]!
                default: break
                }
            }
            return output
        }
        for i in roles.indices {
            let role = roles[i], decorated = role > 99
            if !decorated { output.auxiliary[i] = 0 }
            let family = decorated ? Int(Double(role) * 0.01) : Int(Double(role) * 0.1)
            let remainder = decorated ? role - family * 100 : role - family * 10
            let style = decorated ? remainder % 10 : remainder
            switch family {
            case 1, 4:
                output.kind[i] = family == 1 ? 1 : 5
                if decorated { output.fill[i] = auxiliary[i] != 1 ? 1 : 0 }
                if style == 1 { output.k1[i] = 1 }
            case 2:
                output.kind[i] = 4
                if decorated { output.fill[i] = 1 }
            case 3:
                output.kind[i] = 2
                if decorated { output.fill[i] = auxiliary[i] != 1 ? 1 : 0 }
                switch style {
                case 1: output.k3[i] = 1
                case 2:
                    output.k2[i] = 1; output.b[i] = 1
                    output.beat2[i] = upFirst[i]; output.beat3[i] = upSecond[i]; output.beat4[i] = upThird[i]
                case 3:
                    output.k2[i] = 1; output.beat2[i] = upStarts[i]
                default: break
                }
            case 5: output.kind[i] = 3
            case 6:
                output.kind[i] = 6
                if (decorated ? remainder / 10 : style) == 1 { output.k1[i] = 1 }
            default: break
            }
        }
        return output
    }

    private static func sum(_ values: [Double], start: Int, end: Int) -> Double {
        guard start >= 0, start <= end, end < values.count else { return 0 }
        return values[start...end].reduce(0, +)
    }
}
