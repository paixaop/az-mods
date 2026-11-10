#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Chooses the high-precision model's start from the preliminary beat envelope.
/// The four-beat lead-in is retained in the encoded source-sample clock.
enum NativeBeatModelWindow {
    static func offset(lowIntegrals: [Double], globalBPM: Float, sampleRate: Int) -> Int {
        guard !lowIntegrals.isEmpty else { return 0 }
        let count = lowIntegrals.count
        var levels = lowIntegrals
        if count > 1 {
            for index in 0..<(count - 1) where index > 0 && levels[index] < 0 {
                levels[index] = levels[index - 1]
            }
        }
        levels[count - 1] = 0
        let maximum = levels.dropLast().reduce(0, max)
        if abs(maximum) > Double.ulpOfOne {
            let scale = 1 / maximum
            for index in 0..<(count - 1) { levels[index] *= scale }
        }
        var changes = [Double](repeating: 0, count: count)
        if count > 2 {
            for index in 1..<(count - 1) { changes[index] = levels[index] - levels[index - 1] }
        }
        var active = false
        var sustained = 0
        var gap = 0
        var latestStart = -1
        var clusteredStart = -1
        var accepted = false
        if count > 2 {
            for index in 0..<(count - 2) {
                var extend = false
                if index == 0 {
                    if levels[0] > 0.4 {
                        latestStart = 0
                        clusteredStart = 0
                        extend = true
                    }
                } else if active {
                    extend = levels[index] > 0.4 ||
                        (levels[index] > 0.2 && levels[index + 1] > 0.2 && levels[index + 2] > 0.2)
                } else if levels[index] > 0.2 && changes[index] > 0.15 {
                    if gap >= 33 || latestStart < 0 { clusteredStart = index }
                    latestStart = index
                    gap = 0
                    extend = true
                }
                if extend {
                    if sustained >= 12 { accepted = true; break }
                    sustained += 1
                    active = true
                }
                if (levels[index] < 0.2 && levels[index + 1] < 0.2 && levels[index + 2] < 0.2) ||
                    (changes[index] < -0.4 && changes[index + 1] < 0.2 && changes[index + 2] < 0.2) {
                    sustained = 0
                    active = false
                    gap += 1
                }
            }
        }
        let chosen = accepted || sustained > 12 ? latestStart : clusteredStart
        let beat = max(0, chosen)
        return max(0, Int(Double(sampleRate) * 60 * Double(beat - 4) / Double(globalBPM)))
    }
}
