import XCTest
@testable import Analysis

final class NativePhraseBoundaryTests: XCTestCase {
    /// These expectations come from executing the original caller and its
    /// numerical helpers on generated profiles, without recordings or labels.
    func testCompleteBoundaryRoutesMatchOriginalExecution() {
        let expected = [
            [1, 17, 33, 49, 81, 113, 129, 161, 177, 209],
            [1, 18, 34, 50, 66, 98, 114, 146, 162, 194, 226],
            [1, 19, 51, 83, 99, 131, 163, 179, 211],
            [1, 20, 36, 68, 80, 112, 116, 148, 160, 192, 208],
            [1, 17, 33, 49, 65, 81, 97, 113, 145, 161, 177, 193, 209, 225],
            [1, 18, 26, 50, 66, 82, 98, 130, 146, 178, 194, 210],
            [1, 35, 51, 67, 115, 131, 143, 163, 179, 195],
            [1, 20, 28, 36, 48, 100, 112, 128, 148, 164, 176, 228],
            [1, 17, 33, 49, 81, 97, 113, 129, 145, 161, 177, 209, 225],
            [1, 18, 26, 34, 66, 82, 114, 130, 146, 162, 194, 210],
            [1, 27, 51, 67, 79, 99, 115, 131, 179, 195, 207, 227],
            [1, 36, 48, 64, 84, 100, 112, 164, 176, 192, 212, 228],
            [1], [1], [1], [1]
        ]
        for sample in expected.indices {
            let count = 256, phase = sample % 4 + 1
            func profile(_ pattern: [Double]) -> [Double] {
                (0..<count).map { pattern[($0 / 16 + sample) % pattern.count] }
            }
            let peaks = profile([0.03, 0.04, 0.82, 0.86, 0.84, 0.3, 0.29, 0.7])
            let low = profile([20, 30, 900, 950, 910, 180, 170, 800])
            let middle = profile([90, 200, 70, 500, 800, 140, 370, 800])
            let all = profile([0.03, 0.08, 0.7, 0.8, 0.65, 0.9, 0.5, 0.8])
            let middlePeaks = profile([0.01, 0.1, 0.8, 0.7, 0.6, 0.05, 0.6, 0.9])
            let opening = profile([0.01, 0.04, 0.6, 0.7, 0.8, 0.2, 0.3, 0.5])
            let vocal = profile([0, 0, 1, 1, 0, 0, 1, 1])
            let changes = [0.0] + (1..<count).map { i -> Double in
                if peaks[i] > peaks[i - 1] * 1.77 { return 1 }
                return peaks[i - 1] > peaks[i] * 2 ? 0.5 : 0
            }
            let high = (0..<(count * 8)).map { ($0 + sample) % 8 == 3 ? 0.8 : 0.02 }
            let bars = (count - phase + 1) / 4
            let hiHat = (0..<bars).map { UInt8(($0 + sample) % 3 == 0 ? 1 : 0) }
            let rhythm = (0..<(bars * 16)).map { UInt8(($0 + sample) % 4 == 0 ? 1 : 0) }
            let beats = NativePhraseFeatures.Beats(
                timing: .init(bpm: 128, offsetMilliseconds: 0, phase: phase),
                coordinates: .init(beatCount: count, high: [], middle: [], low: []),
                initialBoundary: 1, lowLevels: low, middleLevels: middle, allLevels: all,
                lowPeaks: peaks, lowPeakChanges: changes, lowQuarterPeaks: [], lowQuarterMean: [],
                middlePeaks: middlePeaks, highEighthPeaks: high,
                lowActivity: NativePhraseFeatures.lowActivity(low, phase: phase),
                middleActivity: NativePhraseFeatures.middleActivity(middle, phase: phase),
                allActivity: NativePhraseFeatures.allActivity(all, phase: phase))
            let actual = NativePhraseBoundaries.candidates(beats: beats,
                openingHypotheses: [3, 4, 5, 6].map { phase + $0 * 4 },
                introEnd: phase + 16, openingFlag: sample < 12 ? 1 : 0,
                openingEnvelope: opening, vocalActivity: vocal, hihatChanges: hiHat,
                rhythm: rhythm, durationSeconds: 120, route: sample < 12 ? sample / 4 + 1 : 1)
            XCTAssertEqual(actual, expected[sample], "generated profile \(sample)")
        }
    }
}
