#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseMood {
    static func maximumLevelRatio(_ quarterPeaks: [Double]) -> Double {
        let bars = quarterPeaks.count / 16
        guard bars > 0 else { return 0 }
        var count = 0
        for bar in 0..<bars where quarterPeaks[(bar * 16)..<(bar * 16 + 16)].max()! >= 0.89125 {
            count += 1
        }
        return Double(count) / Double(bars)
    }

    static func fadeOut(low: [Double], full: [Double]) -> Int {
        guard low.count >= 1552, full.count >= low.count else { return 0 }
        let start = (low.count & ~15) - 528
        var lowBars = [Double](), fullBars = [Double]()
        for bar in 0..<32 {
            let range = (start + bar * 16)..<(start + bar * 16 + 16)
            lowBars.append(low[range].reduce(0, +))
            fullBars.append(full[range].reduce(0, +))
        }
        let pairs = (0..<16).map { fullBars[$0 * 2] + fullBars[$0 * 2 + 1] }
        let ratios = pairs.contains(where: { $0 < 1e-7 })
            ? [Double](repeating: 0, count: 15)
            : (0..<15).map { (lowBars[$0 * 2] + lowBars[$0 * 2 + 1]) / pairs[$0] }
        let minimum = ratios.prefix(12).min()!
        let maximum = ratios.prefix(12).max()!
        return pairs[15] < pairs.max()! * 0.001 && pairs[15] < pairs[14]
            && pairs[14] < pairs[13] && pairs[13] < pairs[12]
            && ratios[12...14].allSatisfy({ $0 >= minimum && $0 <= maximum }) ? 1 : 0
    }

    static func reviseMid(_ mood: PhraseMood, beatCount: Int, lowPeaks: [Double],
                          quarterPeaks: [Double]) -> PhraseMood {
        guard mood == .mid, beatCount >= 96 else { return mood }
        let from = 8, to = beatCount - 64
        let lowMean = lowPeaks[(from - 1)..<(to - 1)].reduce(0, +) / Double(to - from)
        let count = (to * 4 - from * 4 - 1 - 32) / 4
        guard count > 6 else { return mood }
        var maxima = [Double](repeating: 0, count: count)
        var correspondence = maxima
        var previous = 0
        for bar in 1..<count {
            let first = from * 4 + (bar - 1) * 4
            let values = Array(quarterPeaks[first..<(first + 4)])
            var index = 0
            for i in 1..<4 where values[i] > values[index] { index = i }
            let position = index + 1, maximum = values[index]
            maxima[bar] = maximum
            let delta = abs(position - previous)
            if delta == 0 { correspondence[bar - 1] = 1 }
            else if delta == 1 || delta == 3 {
                correspondence[bar - 1] = quarterPeaks[first - 1 + previous] / (maximum == 0 ? 0.003 : maximum)
            }
            previous = position
        }
        guard correspondence.reduce(0, +) / Double(count) > 0.91 else { return mood }
        var changes = 0.0
        for bar in 2..<(count - 4) {
            if maxima[bar] == 0 { maxima[bar] = 0.003 }
            if maxima[bar - 1] == 0 { maxima[bar - 1] = 0.003 }
            changes += max(maxima[bar], maxima[bar - 1]) / min(maxima[bar], maxima[bar - 1])
        }
        return changes / Double(count - 6) >= 1.5 && lowMean >= 0.2 ? .high : mood
    }
}
