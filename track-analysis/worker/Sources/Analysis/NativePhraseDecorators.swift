#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseDecorators {
    struct Decorated {
        let boundaries: [Int]
        let fills: NativePhraseFills.Result
        let upStarts: [Int]
        let upFirst: [Int]
        let upSecond: [Int]
        let upThird: [Int]
    }
    struct UpAnalysis {
        var roles: [Int]
        var starts: [Int]
        var metrics: [Double]
    }
    struct RhythmAnalysis {
        var roles: [Int]
        var starts: [Int]
        var first: [Int]
        var second: [Int]
        var third: [Int]
    }

    /// Exact high-mode feature order. `boundaries` has one entry per role;
    /// Outro analysis supplies the final interval end before final adjustment.
    static func high(boundaries: [Int], roles: [Int], profile: NativePhraseFills.Profile,
                     firstLevel: [Double], secondLevel: [Double], phraseMetric: [Double], activity: [UInt8]) -> Decorated {
        precondition(boundaries.count == roles.count && phraseMetric.count == roles.count)
        var ends = boundaries + [profile.low.count + 1]
        var result = NativePhraseFills.attach(boundaries: ends, roles: roles, profile: profile, stage: .intro)
        if roles.count > 1 {
            for i in 0..<(roles.count - 1) where result.roles[i] == 10 || result.roles[i] == 110 {
                if beatFour(start: boundaries[i], end: boundaries[i + 1], lastBoundary: i == roles.count - 2,
                    low: profile.low, quarters: profile.lowQuarters) { result.roles[i] += 1 }
            }
        }
        let saved = result.roles
        let loop = simpleLoops(boundaries: boundaries, roles: result.roles, firstLevel: firstLevel, secondLevel: secondLevel, phraseMetric: phraseMetric)
        let pattern = rhythm(boundaries: boundaries, roles: loop.roles, activity: activity)
        result.roles = restoreLoops(roles: pattern.roles, saved: saved, metric: loop.metrics)
        let candidates = NativePhraseFills.attach(boundaries: ends, roles: result.roles, profile: profile, stage: .upAndHook)
        result.roles = candidates.roles
        for i in result.roles.indices where candidates.starts[i] != 0 {
            result.starts[i] = candidates.starts[i]; result.routes[i] = candidates.routes[i]
        }
        if roles.count > 2 {
            for i in 1..<(roles.count - 1) where result.roles[i] == 40 || result.roles[i] == 440 {
                if beatFour(start: boundaries[i], end: boundaries[i + 1], lastBoundary: i == roles.count - 2,
                    low: profile.low, quarters: profile.lowQuarters) { result.roles[i] += 1 }
            }
        }
        let ending = outro(boundaries: boundaries, roles: result.roles, low: profile.low,
            activity: activity, quarters: profile.lowQuarters, phase: profile.phase)
        result.roles = ending.roles; ends[ends.count - 1] = ending.terminal
        NativePhraseFills.cleanActivity(boundaries: ends, profile: profile, result: &result)
        NativePhraseFills.finalAdjust(boundaries: ends, highMood: true, phraseMetric: phraseMetric, result: &result)
        return Decorated(boundaries: ends, fills: result, upStarts: pattern.starts,
            upFirst: pattern.first, upSecond: pattern.second, upThird: pattern.third)
    }

    static func dividedBands(samples: NativePhraseFeatures.Samples, coordinates: NativePhraseFeatures.Coordinates) -> (low: [Double], middle: [Double]) {
        func measure(_ samples: [Double], rate: Double, highPass: Bool, positions: [Int]) -> [Double] {
            var filter = NativePhraseEnvelopes.secondOrder(rate: rate, angularCutoff: 3141.592653589793,
                damping: 1.4142135623730951, highPass: highPass)
            var smoother = NativePhraseEnvelopes.smoother(rate: rate, angularCutoff: 31.41592653589793)
            let envelope = smoother.process(filter.process(samples).map(abs))
            return positions.indices.map { i in
                let end = i + 1 < positions.count ? positions[i + 1] - 1 : envelope.count
                return NativePhraseFeatures.sum(envelope, start: positions[i] - 1, count: end - positions[i] + 1)
            }
        }
        return (measure(samples.low, rate: samples.sourceRate / 32, highPass: false,
                    positions: Array(coordinates.low.prefix(coordinates.beatCount))),
                measure(samples.middle, rate: samples.sourceRate / 8, highPass: true,
                    positions: Array(coordinates.middle.prefix(coordinates.beatCount))))
    }

    static func beatFour(start: Int, end: Int, lastBoundary: Bool, low: [Double], quarters: [Double]) -> Bool {
        guard start >= 1, end > start, end - 2 < low.count else { return false }
        let mean = NativePhraseFeatures.sum(low, start: start - 1, count: end - start) / Double(end - start)
        var quarterCount = (end - start) * 4 - 1
        if quarterCount > 48 && !lastBoundary { quarterCount -= 32 }
        let count = quarterCount / 4
        guard count > 0, (start - 1 + count) * 4 <= quarters.count else { return false }
        var positions = [Int](repeating: 0, count: count)
        var amplitudes = [Double](repeating: 0, count: count)
        var ratios = [Double](repeating: 0, count: count)
        if count > 1 {
            for i in 1..<count {
                let base = (start - 1 + i) * 4
                var peak = 0
                for j in 1..<4 where quarters[base + j] > quarters[base + peak] { peak = j }
                positions[i] = peak + 1; amplitudes[i] = quarters[base + peak]
                let distance = abs(positions[i] - positions[i - 1])
                if distance == 0 { ratios[i - 1] = 1 }
                else if distance == 1 || distance == 3 {
                    ratios[i - 1] = quarters[base - 1 + positions[i - 1]] / (amplitudes[i] == 0 ? 0.003 : amplitudes[i])
                }
            }
        }
        var accepted = NativePhraseFeatures.sum(ratios, start: 0, count: count) / Double(count) > 0.8
        let stabilityCount = max(0, count - 4)
        if !accepted && stabilityCount > 2 {
            var total = 0.0
            for i in 2..<stabilityCount {
                if amplitudes[i] == 0 { amplitudes[i] = 0.003 }
                if amplitudes[i - 1] == 0 { amplitudes[i - 1] = 0.003 }
                total += max(amplitudes[i] / amplitudes[i - 1], amplitudes[i - 1] / amplitudes[i])
            }
            accepted = total / Double(stabilityCount - 2) < 1.12
        }
        return accepted && mean >= 0.22
    }

    static func simpleLoops(boundaries: [Int], roles: [Int], firstLevel: [Double], secondLevel: [Double], phraseMetric: [Double]) -> UpAnalysis {
        precondition(boundaries.count == roles.count && phraseMetric.count == roles.count)
        var output = UpAnalysis(roles: roles, starts: Array(repeating: 0, count: roles.count), metrics: Array(repeating: 0, count: roles.count))
        let ends = Array(boundaries.dropFirst()) + [firstLevel.count + 1]
        func average(_ values: [Double], _ start: Int) -> Double { NativePhraseFeatures.sum(values, start: start - 1, count: 8) * 0.125 }
        for i in roles.indices where roles[i] == 30 || roles[i] == 50 {
            let start = boundaries[i], end = ends[i], length = end - start
            guard start > 0, end - 2 < min(firstLevel.count, secondLevel.count) else { continue }
            var maximum = 0.0, minimum = 0.0, maximumAt = end - 8, minimumAt = start
            var firstDifference = 0.0, secondDifference = 0.0, slope = 0.0
            if length >= 16 {
                let maximumBegin = length >= 32 ? start + length / 2 : start
                for position in maximumBegin...(end - 8) {
                    let value = average(secondLevel, position)
                    if value > maximum { maximum = value; maximumAt = position }
                }
                minimum = maximum
                let minimumEnd = length >= 32 ? start + length / 2 - 2 : end - 8
                if minimumEnd >= start {
                    for position in start...minimumEnd {
                        let value = average(secondLevel, position)
                        if value < minimum { minimum = value; minimumAt = position }
                    }
                }
                output.metrics[i] = maximum
                firstDifference = (NativePhraseFeatures.sum(firstLevel, start: maximumAt - 1, count: 8)
                    - NativePhraseFeatures.sum(firstLevel, start: minimumAt - 1, count: 8)) * 0.125
                secondDifference = maximum - minimum
                if maximumAt != minimumAt { slope = secondDifference / Double(maximumAt - minimumAt) }
            }
            guard phraseMetric[i] < 0.16 else { continue }
            let primary = firstDifference < 10 && secondDifference > 85 && slope > 3.5 && slope < 22
            let alternate = secondDifference - firstDifference > 150 && secondDifference > 85 && slope < 22
            let secondary = secondDifference > 85 && slope > 6 && slope < 22
            if primary || alternate || secondary { output.roles[i] = 31; output.starts[i] = start }
        }
        return output
    }

    static func restoreLoops(roles: [Int], saved: [Int], metric: [Double]) -> [Int] {
        precondition(roles.count == saved.count && roles.count == metric.count)
        var result = roles, remaining = 0, runEnd = roles.count
        var spans = [Int](repeating: 0, count: roles.count)
        guard roles.count > 1 else { return result }
        for i in 0..<(roles.count - 1) where roles[i] == 31 {
            for next in (i + 1)..<roles.count where roles[next] != 31 { runEnd = next + 1; break }
            if remaining == 0 { remaining = runEnd - (i + 1); spans[i] = remaining }
            remaining -= 1
        }
        for i in 0..<(roles.count - 1) where spans[i] > 1 {
            let end = i + spans[i]
            var selected = i
            for j in (i + 1)..<end where metric[j] > metric[selected] { selected = j }
            for j in i..<end where j != selected { result[j] = saved[j] }
        }
        return result
    }

    static func rhythm(boundaries: [Int], roles: [Int], activity: [UInt8]) -> RhythmAnalysis {
        precondition(boundaries.count == roles.count)
        let n = roles.count
        var result = RhythmAnalysis(roles: roles, starts: Array(repeating: 0, count: n), first: Array(repeating: 0, count: n), second: Array(repeating: 0, count: n), third: Array(repeating: 0, count: n))
        guard n > 1 else { return result }
        let four = (0..<16).map { UInt8($0 % 4 == 0 ? 1 : 0) }
        let eight = (0..<16).map { UInt8($0 % 2 == 0 ? 1 : 0) }
        var firstEight = 0, afterEights = 0, precedingFour = 0
        for i in 0..<(n - 1) where [10, 11, 30, 31, 50, 51].contains(roles[i] % 100) {
            let start = boundaries[i], length = boundaries[i + 1] - start, groups = length / 4
            guard groups > 0, start > 0, (start - 1) * 4 + groups * 16 <= activity.count else { continue }
            var codes = [Int](repeating: 0, count: groups + 8)
            for g in 0..<groups {
                let base = (start - 1) * 4 + g * 16
                let window = Array(activity[base..<(base + 16)])
                if window == four { codes[g] = 4 }
                if window == eight { codes[g] = 8 }
                else if g > 1 && codes[g - 1] == 8 {
                    let range = codes[g - 2] == 8 ? (base + 2)..<(base + 4) : base..<(base + 4)
                    if activity[range].allSatisfy({ $0 == 1 }) { codes[g] = 16 }
                }
            }
            var found = false, tail = false, preceding = false
            for g in 0..<groups where codes[g] == 8 {
                if found { break }
                firstEight = g + 1; found = true
                if firstEight < groups {
                    if codes[g + 1] == 8 {
                        if codes[g + 2] == 8 {
                            if g + 5 < groups { afterEights = g + 5; tail = true }
                        } else if g + 3 < groups { afterEights = g + 3; tail = true }
                    } else {
                        preceding = firstEight > 1
                        if firstEight >= 2 { precedingFour = g }
                        tail = true; afterEights = g + 2
                    }
                }
            }
            if tail, afterEights <= groups, afterEights > 0, codes[afterEights - 1] == 16 {
                let count = afterEights - firstEight
                let offset = count == 1 ? 8 : (count < 3 ? 16 : 32)
                let second = start + firstEight * 4 - 4
                let proposed = second - offset
                result.roles[i] = 32; result.starts[i] = proposed >= start ? proposed : second - 4
                result.first[i] = result.starts[i]; result.second[i] = second
                result.third[i] = start + (afterEights - 1) * 4
            }
            if result.roles[i] != 32, preceding, precedingFour > 0, length - 1 > 34,
               codes[precedingFour - 1] == 4, precedingFour == groups - 2 {
                result.roles[i] = 32; result.starts[i] = start + (precedingFour - 1) * 4
                result.first[i] = result.starts[i]; result.second[i] = result.starts[i] + 4; result.third[i] = result.starts[i] + 8
            }
        }
        for hook in 1..<n where roles[hook] == 40 {
            let i = hook - 1, baseRole = roles[i] % 100
            let length = boundaries[hook] - boundaries[i], groups = length / 4
            guard result.roles[i] != 32,
                  [30, 31, 50, 51].contains(baseRole) || (length > 35 && (baseRole == 10 || baseRole == 11)),
                  groups > 0, boundaries[i] > 0, (boundaries[i] - 1) * 4 + groups * 16 <= activity.count else { continue }
            let counts = (0..<groups).map { g in
                let p = (boundaries[i] - 1) * 4 + g * 16
                return activity[p..<(p + 16)].reduce(0) { $0 + Int($1) }
            }
            let factor = baseRole == 10 || baseRole == 11 ? 0.85 : 0.9
            func average(_ from: Int, _ to: Int, scale: Double) -> Double {
                counts[(groups + from)...(groups + to)].reduce(0.0) { $0 + Double($1) } * scale
            }
            var staged = false, simple = false, extent = 0, secondOffset = 0, thirdOffset = 0
            if length >= 32 && length < 64 {
                let a = average(-8, -5, scale: 0.25), b = average(-4, -3, scale: 0.5), c = Double(counts[groups - 2])
                staged = a <= b * factor && b <= c * factor
                simple = !staged && a < (b + c) * 0.5
                extent = 32; secondOffset = 16; thirdOffset = 8
            } else if length >= 64 {
                let a = average(-16, -9, scale: 0.125), b = average(-8, -5, scale: 0.25)
                let c = average(-4, -2, scale: 0.33333)
                staged = a <= b * factor && b <= c * factor
                let prior = counts.dropLast()
                simple = !staged && ((prior.max()! - prior.min()! < 5) ? b <= c * factor : a < (c + b) * 0.5)
                extent = staged ? 48 : 32; secondOffset = 32; thirdOffset = 16
            } else if length >= 16 {
                let a = average(-4, -3, scale: 0.5), b = Double(counts[groups - 2])
                staged = a <= b * factor; simple = !staged && a < b
                extent = 16; secondOffset = 16; thirdOffset = 8
            }
            if staged || simple {
                result.roles[i] = staged ? 32 : 33
                result.starts[i] = boundaries[hook] - extent
                result.first[i] = staged ? result.starts[i] : 0
                result.second[i] = staged ? boundaries[hook] - secondOffset : 0
                result.third[i] = staged ? boundaries[hook] - thirdOffset : 0
            }
        }
        for i in 0..<(n - 1) where (roles[i] % 100 == 10 || roles[i] % 100 == 11) && (result.roles[i] == 32 || result.roles[i] == 33) {
            result.roles[i] = roles[i]
        }
        return result
    }

    static func outro(boundaries: [Int], roles: [Int], low: [Double], activity: [UInt8], quarters: [Double], phase: Int) -> (roles: [Int], terminal: Int) {
        precondition(boundaries.count == roles.count)
        guard let lastStart = boundaries.last, !roles.isEmpty else { return (roles, low.count) }
        var low = low, selected = 0
        if roles.last == 60 {
            let count = activity.count / 4 - lastStart + 1
            if count > 0 {
                var lastActive = count
                for i in stride(from: count - 1, through: 0, by: -1) {
                    let base = (lastStart - 1 + i) * 4
                    if activity[base..<(base + 4)].contains(where: { $0 != 0 }) { lastActive = i + 1; break }
                }
                if lastActive > count - 13 && lastActive != count { selected = lastActive + lastStart }
            }
        }
        if selected != 0, selected >= 5, selected - 1 < low.count {
            if low[selected - 1] == 0 { low[selected - 1] = 0.003 }
            if low[selected - 2] == 0 { low[selected - 2] = 0.003 }
            if low[selected - 1] / low[selected - 2] > 0.8
                || NativePhraseFeatures.sum(low, start: selected - 5, count: 4) * 0.25 < 0.05 { selected = 0 }
        }
        let start = lastStart + 9
        var lastAudible = low.count
        if low.count >= start {
            for i in stride(from: low.count - 1, through: start - 1, by: -1) where low[i] > 0.0007 {
                lastAudible = i + 2; break
            }
        }
        let count = max(0, lastAudible - start)
        var samples = start > 0 && start - 1 < low.count ? Array(low[(start - 1)...]) : []
        var ratios = [Double](repeating: 0, count: count)
        var accelerations = [Double](repeating: 0, count: count)
        if count > 1, samples.count >= count {
            for i in 1..<count {
                if samples[i] == 0 { samples[i] = 0.003 }
                if samples[i - 1] == 0 { samples[i - 1] = 0.003 }
                ratios[i] = samples[i] / samples[i - 1]
                if i > 1 { accelerations[i] = ratios[i] / ratios[i - 1] }
            }
            for i in 1..<count where ratios[i] < 0.5 {
                let supported = ((i + 1)..<count).filter { samples[$0] > 0.001 }
                let next = supported.map { ratios[$0] }
                guard next.isEmpty || NativePhraseFeatures.sum(next, start: 0, count: next.count) / Double(next.count) <= 0.86 else { continue }
                guard !supported.contains(where: { ratios[$0] > 1.1 }) else { continue }
                let candidate = start + i
                if selected == 0 || candidate < selected {
                    selected = candidate
                    if candidate < lastAudible - 3 {
                        let remainder = (candidate + 1 - phase) % 4
                        if remainder == 2, i > 1, ratios[i - 1] > 1.3 || ratios[i - 2] > 1.3 { selected -= 2 }
                        else if remainder == 3, i > 2, ratios[i - 2] > 1.3, ratios[i - 3] > 1.3 { selected -= 3 }
                    }
                }
            }
            for i in 1..<count where samples[i] > 0.2 && ratios[i] > 2 {
                var j = i + 1
                while j < count && samples[j] >= 0.0007 && ratios[j] <= 1 { j += 1 }
                if j == count || samples[j] < 0.0007 {
                    let candidate = start + i
                    if selected == 0 || candidate < selected { selected = candidate }
                    break
                }
            }
        }
        if count > 3 {
            for i in 1..<(count - 2) where accelerations[i] > 1.3 && accelerations[i + 1] < 1.03 && accelerations[i + 2] < 1.03 {
                if count <= i + 4 { break }
                if accelerations[(i + 3)..<count].allSatisfy({ $0 <= 0.91 }) {
                    let candidate = start + i
                    if selected == 0 || candidate < selected { selected = candidate }
                    break
                }
            }
        }
        var terminal = selected == 0 ? lastAudible : selected
        if (terminal - phase + 1) & 3 == 0 && terminal < lastAudible { terminal += 1 }
        var updatedRoles = roles
        if beatFour(start: lastStart, end: terminal, lastBoundary: true, low: low, quarters: quarters) {
            updatedRoles[updatedRoles.count - 1] += 1
        }
        return (updatedRoles, terminal)
    }
}
