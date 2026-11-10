#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Fill decisions in the analysis coordinate system. Boundaries are one-based
/// beat ordinals; a per-beat profile's first element belongs to beat one. An
/// absent fill uses zero, and a retained start can have suppressed presence.
enum NativePhraseFills {
    /// Candidate bar ordinals from the phase-shifted upper-band envelope. The
    /// final four bars are reserved, and the output retains its zero padding.
    static func snareCandidates(normalizedHigh: [Double], bars: Int) -> [Int] {
        precondition(bars >= 0 && normalizedHigh.count >= max(0, bars - 4) * 32)
        var output = [Int](repeating: 0, count: bars)
        guard bars >= 6 else { return output }
        var levels = [Double](repeating: 0, count: bars)
        for bar in 0..<(bars - 4) {
            let heights = NativePhraseSpectra.peaks(Array(normalizedHigh[(bar * 32)..<(bar * 32 + 32)])).map(\.height)
            levels[bar] = NativePhraseFeatures.sum(heights, start: 0, count: heights.count)
        }
        let maximum = levels.max()!
        let scale = maximum == 0 ? 333.3333333333333 : 1 / maximum
        levels = levels.map { $0 * scale == 0 ? 0.003 : $0 * scale }
        var ratios = [Double](repeating: 0, count: bars)
        var rising = ratios, falling = ratios, risingCount = 0, fallingCount = 0
        for i in 1..<bars {
            ratios[i] = levels[i] / levels[i - 1]
            if ratios[i] >= 1 { rising[i] = ratios[i]; risingCount += 1 }
            else if ratios[i] != 0 { falling[i] = ratios[i]; fallingCount += 1 }
        }
        var upper = risingCount == 0 ? 0.00345
            : NativePhraseFeatures.sum(rising, start: 0, count: bars) * 1.15 / Double(risingCount)
        var lower = fallingCount == 0 ? 0.00255
            : NativePhraseFeatures.sum(falling, start: 0, count: bars) * 0.85 / Double(fallingCount)
        if bars >= 16 {
            let percentile = bars / 16 - 1
            upper = max(upper, rising.sorted(by: >)[percentile])
            // Both rank adjustments use the rising population. The native
            // routine replaces its falling scratch array before sorting it.
            let ascending = rising.filter { $0 != 0 }.sorted()
            if percentile < ascending.count { lower = min(lower, ascending[percentile]) }
        }
        var skip = false, count = 0
        for i in 1..<(bars - 4) {
            if skip { skip = false; continue }
            let current = ratios[i], next = ratios[i + 1]
            guard current > upper || current < lower,
                  ratios[i + 2] > 0.85, ratios[i + 2] < 1.15 else { continue }
            if current < 1, next > lower, levels[i - 1] > (current <= 0.5 ? 0.5 : 0.3) {
                output[count] = next * current < 0.8 ? i + 1 : i + 2
                count += 1; skip = true
            }
            if current > 1, next < upper, levels[i] > (current >= 2 ? 0.5 : 0.4) {
                output[count] = next * current > 0.8 ? i + 1 : i + 2
                count += 1; skip = true
            }
        }
        return output
    }

    struct Profile {
        let low: [Double]
        let lowQuarterMean: [Double]
        let highEighths: [Double]
        let powerQuarters: [Double]
        let phase: Int
        let lowQuarters: [Double]

        init(low: [Double], lowQuarterMean: [Double], highEighths: [Double], powerQuarters: [Double], phase: Int, lowQuarters: [Double] = []) {
            self.low = low; self.lowQuarterMean = lowQuarterMean; self.highEighths = highEighths
            self.powerQuarters = powerQuarters; self.phase = phase; self.lowQuarters = lowQuarters
        }
    }

    enum Route: Equatable { case growth, cut, repeatedUpper }
    enum AttachmentStage { case all, intro, upAndHook }
    struct Detection: Equatable {
        let start: Int
        let auxiliaryEnd: Int
        let route: Route
    }
    struct Result {
        var roles: [Int]
        var starts: [Int]
        var auxiliary: [Int]
        var routes: [Route?]
        var present: [Bool] {
            roles.indices.map { Self.isDecorated(roles[$0]) && auxiliary[$0] != 1 }
        }
        private static func isDecorated(_ role: Int) -> Bool {
            role == 110 || role == 111 || (330...333).contains(role) || role == 440 || role == 441
        }
    }

    struct HighProfile {
        let values: [Double]
        let supportedBeats: Int
    }

    static func normalizeHigh(_ input: [Double], phase: Int) -> HighProfile? {
        let shift = (phase - 1) * 8
        guard shift >= 0, shift < input.count else { return nil }
        var output = input
        let count = input.count - shift
        let maximum = input[shift...].max() ?? 0
        let factor = maximum == 0 ? 333.3333333333333 : 1 / maximum
        // The terminal three-beat guard is intentionally neither shifted nor
        // normalized. The copied array retains its original length.
        if count > 24 {
            for i in 0..<(count - 24) { output[i] = input[i + shift] * factor }
        }
        return HighProfile(values: output, supportedBeats: count / 8)
    }

    static func growth(start: Int, end: Int, low: [Double]) -> Detection? {
        guard end - start >= 16, end >= 17, end - 2 < low.count else { return nil }
        let means = (0..<4).map { group in
            low[(end - 17 + group * 4)...(end - 14 + group * 4)].reduce(0, +) / 4
        }
        let candidate: Int
        if means[0] * 0.5 <= means[1] {
            guard means[2] < means[1] * 0.5, means[3] > means[2] * 1.25 else { return nil }
            candidate = end - 4
        } else if means[2] > means[1] * 1.25 {
            candidate = end - 8
        } else {
            guard means[3] > means[2] * 1.25 else { return nil }
            candidate = end - 4
        }
        return Detection(start: candidate, auxiliaryEnd: end - 1, route: .growth)
    }

    static func cut(start: Int, end: Int, low: [Double], lowQuarterMean: [Double]) -> Detection? {
        guard end - start >= 16, end >= 17, end - 2 < low.count else { return nil }
        let means = (0..<4).map { group in
            low[(end - 17 + group * 4)...(end - 14 + group * 4)].reduce(0, +) * 0.25
        }
        for group in 1..<4 where means[group] < means[group - 1] * 0.5 {
            return Detection(start: end - 16 + group * 4, auxiliaryEnd: end - 1, route: .cut)
        }
        guard end - 1 < lowQuarterMean.count else { return nil }
        var fine = lowQuarterMean
        for i in (end - 13)..<(end - 6) {
            if fine[i] == 0 { fine[i] = 0.003 }
            let ratio = fine[i + 1] / fine[i]
            guard ratio >= 0.7, ratio <= 1.5 else { return nil }
        }
        for i in (end - 5)...(end - 2) {
            let previous = fine[i - 1], current = fine[i], next = fine[i + 1]
            let sum = fine[(i - 4)...(i - 1)].reduce(0, +)
            let decrease = current < previous * 0.66 && current < sum * 0.165
                && next * current < current * 0.8 * previous
            let increase = current > previous + previous && current > sum * 0.5
                && next * current > current * (previous + previous)
            if decrease || increase {
                return Detection(start: i + 1, auxiliaryEnd: end - 1, route: .cut)
            }
        }
        return nil
    }

    static func repeatedPeakScore(_ values: ArraySlice<Double>) -> Int {
        let samples = Array(values)
        guard samples.count >= 3 else { return 0 }
        let peaks = (1..<(samples.count - 1)).filter {
            samples[$0] > samples[$0 - 1] && samples[$0] >= samples[$0 + 1]
        }
        guard peaks.count >= 3 else { return 0 }
        var amplitudes = [Double](repeating: 0, count: 8)
        var spacings = [Int](repeating: 0, count: 8)
        var run = 0
        for (a, b) in zip(peaks, peaks.dropFirst()) {
            let spacing = b - a, ratio = samples[b] / samples[a]
            if (2...4).contains(spacing), ratio > 0.7, ratio < 1.43 {
                amplitudes[run] = samples[a]
                spacings[run] = spacing
                run += 1
            } else {
                // The scratch amplitudes survive a reset. This affects the
                // maximum used to qualify a later, shorter run.
                run = 0
            }
        }
        guard run >= 2 else { return 0 }
        amplitudes.append(samples[peaks.last!])
        let maximum = amplitudes.max() ?? 0
        guard maximum >= 0.1 else { return 0 }
        if maximum < 0.35 {
            for i in 1..<run where spacings[i] != spacings[i - 1] { return 0 }
        }
        return run
    }

    static func repeatedScores(_ high: HighProfile) -> [Int] {
        let count = high.supportedBeats / 4
        var scores = [Int](repeating: 0, count: count)
        guard high.supportedBeats >= 20 else { return scores }
        for group in 0..<(count - 4) {
            let start = group * 32 + 16
            guard start + 16 <= high.values.count else { break }
            scores[group] = repeatedPeakScore(high.values[start..<(start + 16)])
        }
        return scores
    }

    static func repeatedUpper(end: Int, origin: Int, scores: [Int]) -> Detection? {
        let delta = end - origin, group = delta / 4
        guard scores.count > 3, delta > 7, group >= 2, group - 1 < scores.count,
              scores[group - 1] > 1, scores[group - 2] < 2 else { return nil }
        return Detection(start: end - 2, auxiliaryEnd: 0, route: .repeatedUpper)
    }

    /// Local detectors run in family-specific order. `roles` includes the final
    /// interval; wrappers never attach a fill to that last interval.
    static func attach(boundaries: [Int], roles: [Int], profile: Profile, stage: AttachmentStage = .all) -> Result {
        precondition(boundaries.count == roles.count + 1)
        var result = Result(roles: roles, starts: Array(repeating: 0, count: roles.count),
            auxiliary: Array(repeating: 0, count: roles.count), routes: Array(repeating: nil, count: roles.count))
        guard roles.count >= 2 else { return result }
        let scores = normalizeHigh(profile.highEighths, phase: profile.phase).map(repeatedScores) ?? []
        func cutAt(_ j: Int) -> Detection? {
            cut(start: boundaries[j - 1], end: boundaries[j], low: profile.low, lowQuarterMean: profile.lowQuarterMean)
        }
        func upperAt(_ j: Int) -> Detection? {
            repeatedUpper(end: boundaries[j], origin: boundaries[0], scores: scores)
        }
        func save(_ detection: Detection?, at i: Int, role: Int) {
            guard let detection else { return }
            result.roles[i] = role; result.starts[i] = detection.start; result.routes[i] = detection.route
        }
        for i in 0..<(roles.count - 1) where stage != .upAndHook && roles[i] == 10 {
            save(cutAt(1) ?? upperAt(i + 1), at: 0, role: 110)
        }
        if roles.count > 2 && stage != .intro {
            for i in 1..<(roles.count - 1) where (30...33).contains(roles[i]) {
                let detection = growth(start: boundaries[i], end: boundaries[i + 1], low: profile.low)
                    ?? cutAt(i + 1) ?? upperAt(i + 1)
                save(detection, at: i, role: roles[i] + 300)
            }
            for i in 1..<(roles.count - 1) where roles[i] == 40 {
                save(cutAt(i + 1) ?? upperAt(i + 1), at: i, role: 440)
            }
        }
        return result
    }

    static func cleanActivity(boundaries: [Int], profile: Profile, result: inout Result) {
        guard result.roles.count > 1, profile.powerQuarters.count >= profile.low.count * 4 else { return }
        let power = profile.low.indices.map { i in profile.powerQuarters[(i * 4)..<(i * 4 + 4)].reduce(0, +) }
        let lowMaximum = profile.low.max() ?? 0, powerMaximum = power.max() ?? 0
        let lowScale = lowMaximum == 0 ? 333.3333333333333 : 1 / lowMaximum
        let powerScale = powerMaximum == 0 ? 333.3333333333333 : 1 / powerMaximum
        let low = profile.low.map { $0 * lowScale }, activity = power.map { $0 * powerScale }
        for i in 0..<(result.roles.count - 1) {
            let end = boundaries[i + 1], start = result.starts[i]
            guard start != 0, end - start > 2, end >= 5, end - 2 < low.count else { continue }
            let quietCount = ((end - 5)...(end - 2)).filter { low[$0] < 0.15 }.count
            guard quietCount > 2, activity[end - 3] < 0.04, activity[end - 4] < 0.04,
                  activity[end - 2] < 0.04 || activity[end - 5] < 0.04 else { continue }
            if start > 8, start - 2 < low.count,
               ((start - 9)...(start - 2)).filter({ low[$0] > 0.4 }).count > 5 {
                result.auxiliary[i] = 1
            } else {
                result.starts[i] = 0
                let role = result.roles[i]
                if role >= 401 { result.roles[i] -= 400 }
                else if role >= 301 { result.roles[i] -= 300 }
                else if role >= 101 { result.roles[i] -= 100 }
            }
        }
    }

    static func isHook(_ role: Int) -> Bool { role == 40 || role == 41 || role == 440 || role == 441 }
    static func isUp(_ role: Int) -> Bool { (30...33).contains(role) || (330...333).contains(role) }
    static func undecorated(_ role: Int) -> Int {
        if role == 110 || role == 111 { return role - 100 }
        if (330...333).contains(role) { return role - 300 }
        if role == 440 || role == 441 { return role - 400 }
        return role
    }

    /// The final neighbor pass consumes roles after the detector wrappers and
    /// activity cleanup. The terminal boundary is included in `boundaries`.
    static func finalAdjust(boundaries: [Int], highMood: Bool, phraseMetric: [Double], result: inout Result) {
        let count = result.roles.count
        precondition(boundaries.count == count + 1 && phraseMetric.count == count)
        guard count >= 2 else { return }
        for i in 1..<count where boundaries[i + 1] - boundaries[i] < 9 && isHook(result.roles[i]) {
            let current = result.roles[i], previous = result.roles[i - 1]
            guard !isHook(previous) else { continue }
            if i == count - 1 {
                result.roles[i] = current == 40 || current == 41 ? 60 : 61
            } else if !isHook(result.roles[i + 1]) {
                if i == 1 {
                    result.roles[i] = [40: 10, 41: 11, 440: 110, 441: 111][current]!
                }
                if previous == 50 { result.roles[i] = previous }
                if isUp(previous) { result.roles[i] = current == 40 ? 30 : 330 }
            }
        }
        guard highMood else { return }
        for i in 1..<count where boundaries[i + 1] - boundaries[i] < 32 {
            let current = result.roles[i]
            guard current == 40 || current == 440 else { continue }
            let preceding = result.roles[i - 1]
            let following = i + 1 < count ? result.roles[i + 1] : 0
            if preceding == 41 || preceding == 441 || following == 41 || following == 441 {
                result.roles[i] = current == 40 ? 41 : 441
            }
        }
        for i in 0..<(count - 1) {
            let currentLength = boundaries[i + 1] - boundaries[i]
            let followingLength = boundaries[i + 2] - boundaries[i + 1]
            let following = result.roles[i + 1]
            var clear = currentLength < 5
            if followingLength < 5 {
                clear = true
                if isHook(following), currentLength > 4, i + 2 < count {
                    clear = !isHook(result.roles[i + 2])
                }
            }
            if following == 50 || following == 60 { clear = true }
            if clear { result.starts[i] = 0; result.roles[i] = undecorated(result.roles[i]) }
        }
        if count > 3 {
            for i in 2..<(count - 1) where boundaries[i + 1] - boundaries[i] < 13 {
                if result.roles[i - 1] == 331 || result.roles[i - 1] == 31 {
                    if result.roles[i] == 330 { result.roles[i] = 331 }
                    else if result.roles[i] == 30 { result.roles[i] = 31 }
                }
            }
        }
        if count > 2 {
            for i in 1..<(count - 1) where boundaries[i + 1] - boundaries[i] > 68 {
                if result.roles[i] == 31 { result.roles[i] = 30 }
                else if result.roles[i] == 331 { result.roles[i] = 330 }
            }
            if (31...33).contains(result.roles[count - 2]) { result.roles[count - 2] = 30 }
        }
        if count > 4 {
            if !result.roles[1..<(count - 1)].contains(where: isHook) {
                for i in 2..<(count - 2) {
                    let previous = result.roles[i - 1]
                    if ((331...333).contains(previous) || previous == 31 || previous == 32), result.roles[i + 1] == 50 {
                        result.roles[i] = result.roles[count - 1] == 61 ? 41 : 40
                    }
                }
            }
            for i in 1..<(count - 1) where phraseMetric[i] >= 0.28 {
                if result.roles[i] == 332 || result.roles[i] == 333 { result.roles[i] = 330 }
                else if result.roles[i] == 32 || result.roles[i] == 33 { result.roles[i] = 30 }
            }
        }
        for i in 0..<(count - 1) where result.starts[i] != 0 && boundaries[i + 1] - result.starts[i] > 4 {
            result.starts[i] = boundaries[i + 1] - 4
        }
    }
}
