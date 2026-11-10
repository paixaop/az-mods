#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseAdditionalBoundaries {
    /// Coarse level changes followed by two stable bars. Returns the native
    /// one-based bar ordinals, with its unused output slots still zero.
    static func noiseChanges(_ levels: [Double]) -> [Int] {
        let count = levels.count / 4
        var means = [Double](repeating: 0, count: count)
        var contrast = means
        if count >= 2 {
            for bar in 0..<(count - 1) {
                means[bar] = levels[(bar * 4)..<(bar * 4 + 4)].reduce(0, +) * 0.25
            }
        }
        if count >= 3 {
            for bar in 1..<(count - 1) {
                if means[bar - 1] == 0 { means[bar - 1] = 0.003 }
                if means[bar] == 0 { means[bar] = 0.003 }
                let ratio = means[bar] / means[bar - 1]
                contrast[bar] = ratio < 1 ? 1 / ratio : ratio
            }
        }
        var result = [Int](repeating: 0, count: count)
        var written = 0
        if count >= 5 {
            for bar in 0..<(count - 4) {
                if contrast[bar + 1] >= 1.8,
                   (contrast[bar + 3] + contrast[bar + 4]) * 0.5 <= 1.11 {
                    result[written] = bar + 2
                    written += 1
                }
            }
        }
        return result
    }
}
