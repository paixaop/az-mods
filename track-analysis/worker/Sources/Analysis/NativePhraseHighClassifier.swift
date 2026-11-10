#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Ordered numerical stage-D coarse roles. Coordinates are native one-based beat
/// starts; the final start has a summary extending to the end of the input.
/// Raw extraction and independent native-execution checks are retained in the
/// private reconstruction ledger; this is an independently written scalar model.
enum NativePhraseHighClassifier {
    struct Result: Equatable {
        let roles: [Int]
        let simpleAlternation: Bool
    }

    static func classify(boundaries: [Int], introMeans: [Double], lowMeans: [Double]) -> Result {
        let n = boundaries.count
        precondition(n > 0 && introMeans.count == n && lowMeans.count == n)
        if n == 1 { return Result(roles: [10], simpleAlternation: true) }
        if n == 2 { return Result(roles: [10, 60], simpleAlternation: true) }
        func reciprocal(_ value: Double) -> Double { value == 0 ? 1 / 0.003 : 1 / value }
        func normalized(_ values: [Double]) -> [Double] {
            let scale = reciprocal(values.max()!)
            return values.map { $0 * scale == 0 ? 0.003 : $0 * scale }
        }
        let normalizedIntro = normalized(introMeans), normalizedLow = normalized(lowMeans)
        var intro = introMeans, low = lowMeans
        var introRatio = [Double](repeating: 0, count: n)
        for i in 1..<n {
            if intro[i - 1] == 0 { intro[i - 1] = 0.003 }
            if intro[i] == 0 { intro[i] = 0.003 }
            introRatio[i] = intro[i] / intro[i - 1]
        }
        var turningPoints = [normalizedIntro[1]]
        if n >= 4 {
            var rising = normalizedIntro[1] < normalizedIntro[2]
            let mean = normalizedIntro.reduce(0, +) / Double(n)
            for i in 2..<(n - 1) {
                if boundaries[i + 1] - boundaries[i] < 16 && normalizedIntro[i] <= mean * 0.65 { continue }
                let nextRising = rising ? normalizedIntro[i] <= normalizedIntro[i + 1] : normalizedIntro[i] < normalizedIntro[i + 1]
                if rising != nextRising { turningPoints.append(normalizedIntro[i]) }
                rising = nextRising
            }
        }
        let largeTurns = zip(turningPoints, turningPoints.dropFirst()).filter { abs($0 - $1) >= 0.3 }.count
        let simple = turningPoints.count <= 3 || largeTurns <= 2
        var lowRatio = [Double](repeating: 0, count: n)
        for i in 1..<n {
            if low[i - 1] == 0 { low[i - 1] = 0.003 }
            lowRatio[i] = low[i] / low[i - 1]
        }
        let meanScale = reciprocal(intro.reduce(0, +) / Double(n))
        var support = [Double](repeating: 0, count: n)
        for i in 1..<(n - 1) {
            support[i] = 1
            if intro[i] != 0 && normalizedLow[i] >= 0.84 {
                if intro[i - 1] == 0 { intro[i - 1] = 0.003 }
                if intro[i + 1] == 0 { intro[i + 1] = 0.003 }
                support[i] = (intro[i] / intro[i + 1] + intro[i] / intro[i - 1]) * intro[i] * meanScale
            }
        }
        let supportScale = reciprocal(support[1..<(n - 1)].reduce(0, +) / Double(n - 2))
        var roles = [Int](repeating: 0, count: n)
        roles[0] = 10; roles[n - 1] = 60
        for i in 1..<(n - 1) {
            if roles[i - 1] == 40 {
                if normalizedLow[i] > 0.7 && introRatio[i] > 0.7 && introRatio[i] < 1.9 { roles[i] = 40 }
            } else {
                let ratio = lowRatio[i]
                if (ratio > 4 && normalizedLow[i] > 0.8 && intro[i] > 0.3)
                    || (ratio > 2 && normalizedLow[i] > 0.5 && intro[i] > 0.75)
                    || (ratio > 1.2 && normalizedLow[i] > 0.65 && intro[i] > 0.45) { roles[i] = 40 }
                if roles[i] == 0 && normalizedLow[i] > 0.8 {
                    if (ratio <= 1.5 && support[i] * supportScale > 1.25)
                        || (ratio > 1.5 && support[i] * supportScale > 1) { roles[i] = 40 }
                }
            }
        }
        let hooks = (0..<(n - 1)).filter { roles[$0] == 40 }
        let hookLowScale = reciprocal(hooks.map { low[$0] }.max() ?? 0)
        let hookIntroScale = reciprocal(hooks.map { intro[$0] }.max() ?? 0)
        for i in hooks where (low[i] * hookLowScale < 0.48 || intro[i] * hookIntroScale < 0.48) && support[i] < 1.1 { roles[i] = 0 }
        for i in 0..<(n - 1) where roles[i] == 40 && roles[i + 1] == 40 && introRatio[i + 1] > 1.4 { roles[i] = 0 }
        if !roles[1..<(n - 1)].contains(40) {
            let ranking = lowRatio[1..<(n - 1)].filter { $0 != 0 }.sorted(by: >)
            var rank = 0, selected = 0
            for i in 1..<(n - 1) {
                if rank < ranking.count && lowRatio[i] == ranking[rank] {
                    if normalizedLow[i] > 0.84 {
                        roles[i] = 40
                        if selected > 2 { break }
                        selected += 1
                    }
                    rank += 1
                }
            }
        }
        for i in 1..<(n - 1) where roles[i] == 0 && roles[i + 1] == 40 { roles[i] = 30 }
        for i in 1..<(n - 1) where roles[i] == 0 && roles[i - 1] == 40 { roles[i] = 50 }
        for i in 1..<(n - 1) where roles[i] == 0 && roles[i - 1] == 10 && normalizedIntro[i] < 0.5 && introRatio[i] < 1.1 { roles[i] = 10 }
        for i in 1..<(n - 1) where roles[i] == 0 {
            if (introRatio[i] > 1 && roles[i + 1] != 60)
                || (boundaries[i + 1] - boundaries[i] < 32 && roles[i - 1] == 30 && roles[i + 1] == 30) { roles[i] = 30 }
        }
        for i in 1..<(n - 1) where roles[i] == 0 && (roles[i + 1] == 60 || introRatio[i] < 1) { roles[i] = 50 }
        if roles[n - 2] == 0 { roles[n - 2] = 50 }
        let hookCount = roles[1..<(n - 1)].filter { $0 == 40 }.count
        for i in 1..<(n - 1) {
            if roles[i] == 50 {
                if roles[i - 1] == 30 && normalizedLow[i] > 0.75 {
                    if normalizedIntro[i] > 0.7 && hookCount < 4 { roles[i] = 40 }
                    else if normalizedIntro[i] > 0.7 && hookCount >= 4 && roles[i + 1] != 60 && roles[i + 1] != 50 { roles[i] = 30 }
                }
            } else if roles[i] == 0 {
                if normalizedLow[i] > 0.75 && normalizedIntro[i] > 0.7 {
                    if hookCount < 4 { roles[i] = 40 }
                    else { roles[i] = roles[i + 1] == 60 || roles[i + 1] == 50 ? 50 : 30 }
                } else if roles[i - 1] == 10 { roles[i] = 30 }
                else { roles[i] = roles[i + 1] == 60 || roles[i + 1] == 50 ? 50 : roles[i - 1] }
            }
        }
        for i in 0..<(n - 1) where roles[i] == 50 && roles[i + 1] != 60 && normalizedLow[i] > 0.9 && lowRatio[i] > 0.9 { roles[i] = 30 }
        return Result(roles: roles, simpleAlternation: simple)
    }

    struct BlockMeans: Equatable { let intro: [Double]; let low: [Double]; let quarterPowerMeans: [Double] }
    static func blockMeans(boundaries: [Int], intro: [Double], low: [Double], quarterPower: [Double], phase: Int) -> BlockMeans {
        precondition(!boundaries.isEmpty && phase >= 1)
        let count = min(intro.count, low.count)
        var limits = boundaries.filter { $0 <= count }
        limits.append(count + 1)
        var introMeans = [Double](repeating: 0, count: boundaries.count), lowMeans = introMeans
        if limits.count > 1 {
            for i in 0..<(limits.count - 1) {
                let start = i == 0 ? max(1, limits[i] - phase + 1) : limits[i] - phase + 1
                let end = limits[i + 1] - phase + 1
                let trim = min(4, Int(Double(end - start) * 0.045 + 0.5)) * 4
                let introRange = (start - 1)..<(end - 1 - trim)
                let lowRange = (limits[i] - 1)..<(limits[i + 1] - 1 - trim)
                precondition(introRange.lowerBound >= 0 && introRange.upperBound <= intro.count && lowRange.lowerBound >= 0 && lowRange.upperBound <= low.count)
                introMeans[i] = introRange.isEmpty ? 0 : intro[introRange].reduce(0, +) / Double(introRange.count)
                lowMeans[i] = lowRange.isEmpty ? 0 : low[lowRange].reduce(0, +) / Double(lowRange.count)
            }
        }
        let reduced = stride(from: 0, to: quarterPower.count / 4 * 4, by: 4).map { quarterPower[$0..<($0 + 4)].reduce(0, +) * 0.25 }
        return BlockMeans(intro: introMeans, low: lowMeans, quarterPowerMeans: reduced)
    }
}
