#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Tests short sequences of onset intervals against quarter-beat subdivisions.
/// Candidate order and strict interval limits are part of the detector.
enum NativeBeatPeriodCandidates {
    struct Result: Equatable, Sendable {
        var periodMilliseconds: Float
        var error: Float
        var matchedIntervals: Int
    }

    private struct Ratio {
        let lower: Float
        let upper: Float
        let beats: Float
    }

    private static let currentRatios: [Ratio] = [
        .init(lower: 500, upper: 1276, beats: 1.5),
        .init(lower: 468, upper: 1071, beats: 1.25),
        .init(lower: 923, upper: 2142, beats: 2.5),
        .init(lower: 290, upper: 638, beats: 0.75),
        .init(lower: 320, upper: 860, beats: 1),
        .init(lower: 290, upper: 428, beats: 0.5),
        .init(lower: 666, upper: 1714, beats: 2),
        .init(lower: 1500, upper: 3333, beats: 4),
        .init(lower: 1125, upper: 2580, beats: 3)
    ]

    private static let previousRatios = [7, 2, 6, 0, 1, 4, 3, 5, 8].map { currentRatios[$0] }

    static func calculate(interval: Float, previous: [Float]) -> Result? {
        var result: Result?
        for ratio in currentRatios where interval > ratio.lower && interval < ratio.upper {
            guard let candidate = candidate(interval: interval, ratio: ratio.beats, previous: previous),
                  candidate.periodMilliseconds >= 333,
                  candidate.matchedIntervals > (result?.matchedIntervals ?? 0) else { continue }
            result = candidate
        }
        return result
    }

    private static func candidate(interval: Float, ratio: Float, previous: [Float]) -> Result? {
        let initialPeriod = interval / ratio
        let tolerance = Float(min(Double(initialPeriod) * 0.04, 20))
        var target: Float = ratio >= 4 ? 8 : 4
        var duration = interval
        var beats = ratio
        var used = 0
        func preceding(_ index: Int) -> Float {
            index < previous.count ? previous[previous.count - 1 - index] : 0
        }
        while used <= 29 {
            let value = preceding(used)
            guard let prior = previousRatios.first(where: {
                value > $0.lower && value < $0.upper && abs(value / $0.beats - initialPeriod) < tolerance
            }) else { return nil }
            duration += value
            beats += prior.beats
            used += 1
            if beats == target {
                let period = duration / target
                let error = abs(period - initialPeriod)
                let tolerance = Float(min(Double(period) * 0.04, 20))
                var matched = used
                while true {
                    let prior = preceding(matched)
                    let quarters = Float(Int(((1 / period) * prior * 4) + 0.5)) * 0.25
                    let ratio = min(quarters > 2.5 ? Float(Int(quarters + 0.5)) : quarters, 4)
                    // A zero-filled history slot yields 0/0 here. The original
                    // ordered comparison accepts that slot before its count guard.
                    if tolerance <= abs(prior / ratio - period) { break }
                    let next = matched + 1
                    if matched > 28 || previous.count <= matched + 2 {
                        matched = next
                        break
                    }
                    matched = next
                }
                return .init(periodMilliseconds: period, error: error, matchedIntervals: matched)
            }
            if beats >= target {
                guard target > 4, target < 16, beats < 16, previous.count > used + 1 else { return nil }
                target += target
            }
            guard previous.count > used + 1 else { return nil }
        }
        return nil
    }
}
