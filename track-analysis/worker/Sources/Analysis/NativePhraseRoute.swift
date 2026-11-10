#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseRoute {
    struct Result {
        let mood: PhraseMood
        let route: Int
        let score: Double
        let positiveProfile: [Double]
        let negativeProfile: [Double]
    }

    static func classify(lowPeaks: [Double], phase: Int, duration: Double, loopTypes: [Int], rhythmTypes: [Int], mood: PhraseMood) throws -> Result {
        guard (1...4).contains(phase), lowPeaks.count >= 8, duration >= 0 else { throw NativePhraseClassifier.InputError.inconsistentDimensions }
        let n = lowPeaks.count / 4
        var bars = [Double](repeating: 0, count: n)
        for i in 0..<(n - 1) { bars[i] = lowPeaks[(phase - 1 + i * 4)...(phase + 2 + i * 4)].reduce(0, +) * 0.25 }
        flatten(&bars)
        let inverted = bars.map { 1 - $0 }
        var filtered = inverted
        var peaks = NativePhraseSpectra.peaks(inverted)
        var left = 0, right = 0
        if peaks.count > 2 {
            for i in stride(from: peaks.count - 2, through: 1, by: -1) {
                if peaks[i].width >= 4 { peaks.remove(at: i); continue }
                left = leftValley(inverted, position: peaks[i].position - 1, fallback: left)
                right = rightValley(inverted, position: peaks[i].position - 1, fallback: right)
                if left + 1 < right { for j in (left + 1)..<right { filtered[j] = filtered[left] } }
            }
        }
        if let first = peaks.first, first.position > 0 {
            left = leftValley(inverted, position: first.position - 1, fallback: 0)
            right = rightValley(inverted, position: first.position - 1, fallback: right, restartFlatTail: true)
            fillEdge(&filtered, original: inverted, left: left, right: right)
            if peaks.count > 1 {
                left = leftValley(inverted, position: peaks.last!.position - 1, fallback: left)
                right = rightValley(inverted, position: peaks.last!.position - 1, fallback: n - 1, restartFlatTail: true)
                fillEdge(&filtered, original: inverted, left: left, right: right)
            }
        }
        filtered = filtered.map { 1 - $0 }
        flatten(&filtered)
        var changes = zip(filtered, filtered.dropFirst()).map { $1 - $0 } + [0]
        if changes.count > 34 { changes = Array(changes[16..<(changes.count - 17)]) }
        changes.sort(by: >)
        let half = changes.count / 2
        var positive = [Double](repeating: 0, count: changes.count), negative = positive
        if half > 0 {
            for i in 0..<half {
                positive[half - 1 - i] = changes[i]
                negative[half - 1 - i] = -changes[changes.count - 1 - i]
            }
        }
        if half > 1 {
            for i in 0..<(half - 1) {
                positive[half + i] = changes[i]
                negative[half + i] = -changes[changes.count - 1 - i]
            }
        }
        let p = NativePhraseSpectra.peaks(positive)[0], q = NativePhraseSpectra.peaks(negative)[0]
        let score = (p.width * p.height).addingProduct(q.height, q.width)
        let long = duration > 600
        let strong = long ? duration * 0.00833 : 3
        let weak = long ? duration * 0.00417 : 1.5
        let loopThreshold = long ? Int(duration * 0.00667) : 2
        let rhythmThreshold = long ? Int(duration * 0.00333) : 1
        let loops = loopTypes.filter { $0 == 31 }.count
        let rhythms = rhythmTypes.indices.dropFirst().filter { rhythmTypes[$0] == 32 || (rhythmTypes[$0] == 33 && rhythmTypes[$0 - 1] != 10) }.count
        if score >= strong || loops >= loopThreshold || rhythms >= rhythmThreshold {
            return Result(mood: .high, route: 1, score: score, positiveProfile: positive, negativeProfile: negative)
        }
        return Result(mood: mood, route: score >= weak ? 1 : 2, score: score, positiveProfile: positive, negativeProfile: negative)
    }

    private static func flatten(_ values: inout [Double]) {
        for i in 1..<values.count {
            let difference = abs(values[i - 1] - values[i])
            if difference < values[i - 1] * 0.1 || difference < 0.05 { values[i] = values[i - 1] }
        }
    }

    private static func leftValley(_ values: [Double], position: Int, fallback: Int) -> Int {
        var i = position
        while i > 0 {
            if !(values[i - 1] < values[i]) { return i }
            i -= 1
        }
        return fallback
    }

    private static func rightValley(_ values: [Double], position: Int, fallback: Int, restartFlatTail: Bool = false) -> Int {
        var i = position
        while i < values.count - 1 && values[i] <= values[i + 1] { i += 1 }
        // A flat tail restarts descent at the peak for edge peaks, and
        // one element after the peak for interior peaks.
        if i == values.count - 1 { i = position + (restartFlatTail ? 0 : 1) }
        while i < values.count - 1 {
            if !(values[i + 1] < values[i]) { return i }
            i += 1
        }
        return fallback
    }

    private static func fillEdge(_ values: inout [Double], original: [Double], left: Int, right: Int) {
        if original[left] <= original[right] {
            if left + 1 < right { for i in (left + 1)..<right { values[i] = values[right] } }
        } else if left < right {
            for i in left..<right { values[i] = values[left] }
        }
    }
}
