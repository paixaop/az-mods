#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Select the onset within a summed, 100 ms window around the provisional beats.
enum NativeBeatAlignment {
    struct Selection: Equatable, Sendable {
        var value = 0.0
        var position = 0
        var rejected = false
    }

    private struct Profile {
        let smooth: [Double]
        let difference: [Double]
        let peaks: [NativeBeatPeaks.Peak]
        let maximum: Double
        let maximumIndex: Int
        let maximumDifference: Double
        let minimumBeforeMaximum: Double
        let minimumIndex: Int

        init(_ values: [Double]) {
            smooth = (0..<(values.count - 32)).map { start in
                values[start..<(start + 32)].reduce(0, +) * 0.03125
            }
            difference = zip(smooth, smooth.dropFirst()).map { $1 - $0 }
            peaks = NativeBeatPeaks.find(smooth)
            var maximum = 0.0, maximumIndex = 0
            for (i, value) in smooth.enumerated() where maximum < value {
                maximum = value; maximumIndex = i
            }
            self.maximum = maximum; self.maximumIndex = maximumIndex
            maximumDifference = max(0, difference.max() ?? 0)
            var minimum = smooth[0], minimumIndex = 0
            if maximumIndex > 1 {
                for index in 1..<maximumIndex where smooth[index] < minimum {
                    minimum = smooth[index]; minimumIndex = index
                }
            }
            minimumBeforeMaximum = minimum; self.minimumIndex = minimumIndex
        }
    }

    static func all(_ values: [Double]) -> Selection {
        guard values.count >= 34 else { return Selection() }
        let profile = Profile(values)
        let peakHeight = max(0, profile.peaks.map(\.height).max() ?? 0)
        let prominence = max(0, profile.peaks.map(\.prominence).max() ?? 0)
        var previous = 0, major = 0
        if let selected = profile.peaks.firstIndex(where: {
            peakHeight * 0.65 < $0.height && prominence * 0.25 < $0.prominence
        }) {
            major = profile.peaks[selected].position
            for peak in profile.peaks.prefix(selected).reversed()
                where peakHeight * 0.25 < peak.height && prominence * 0.25 < peak.prominence {
                previous = peak.position
            }
        }
        var result = Selection(), run = 0
        if previous < major {
            for index in previous..<major {
                if profile.maximumDifference * 0.25 < profile.difference[index],
                   profile.maximum * 0.4 < profile.smooth[index], profile.minimumIndex < index {
                    if run > 6 {
                        result.position = index + 8
                        result.value = values[result.position]
                        break
                    }
                    run += 1
                } else { run = 0 }
            }
        }
        var maximum = 0.0, maximumIndex = 0
        for (i, value) in values.enumerated() where maximum < value {
            maximum = value; maximumIndex = i
        }
        if (result.position == 0 && result.value == 0) || values.allSatisfy({ maximum * 0.7 <= $0 }) {
            result.position = maximumIndex; result.value = maximum
        }
        return result
    }

    static func each(_ values: [Double], allMaximum: Double) -> Selection {
        guard values.count >= 34 else { return Selection() }
        let profile = Profile(values)
        let mean = sumEight(profile.difference) / Double(profile.difference.count)
        let variance = sumEight(profile.difference, squaredDistanceFrom: mean)
        let relativeSlope = abs(allMaximum) > Double.ulpOfOne ? profile.maximumDifference / allMaximum : 0
        let relativeVariance = abs(allMaximum) > Double.ulpOfOne
            ? variance / (allMaximum * Double(profile.difference.count)) : 0
        let height = max(0, profile.peaks.map(\.height).max() ?? 0)
        let prominence = max(0, profile.peaks.map(\.prominence).max() ?? 0)
        let major = profile.peaks.first(where: {
            height * 0.75 < $0.height && prominence * 0.25 < $0.prominence
        })?.position ?? 0
        let threshold = profile.minimumBeforeMaximum <= profile.maximum * 0.4
            ? profile.maximum * 0.4
            : profile.minimumBeforeMaximum.addingProduct(profile.maximum - profile.minimumBeforeMaximum, 0.4)
        var result = Selection(), levelPosition = 0
        let start = profile.minimumIndex + 1
        if start < major {
            var run = 0
            for index in start..<major {
                if profile.difference[index] > profile.maximumDifference * 0.25,
                   profile.smooth[index] > threshold {
                    if run > 3 {
                        result.position = index + 11
                        result.value = values[result.position]
                        break
                    }
                    run += 1
                } else { run = 0 }
            }
            run = 0
            for index in start..<major {
                if threshold < profile.smooth[index] {
                    if run > 8 { levelPosition = index + 6; break }
                    run += 1
                } else { run = 0 }
            }
        }
        let minimum = values.min() ?? 0
        var maximum = values[0], maximumIndex = 0
        for index in 1..<values.count where maximum < values[index] {
            maximum = values[index]; maximumIndex = index
        }
        let baselineRatio = abs(maximum) > Double.ulpOfOne ? minimum / maximum : 0
        let contrast = abs(maximum) > Double.ulpOfOne ? (maximum - minimum) / maximum : 0
        let tailMinimum = values[maximumIndex...].min() ?? 0
        let range = maximum - tailMinimum
        let activeTail = values[maximumIndex...].map { $0 - tailMinimum }.filter { range * 0.25 < $0 }
        let tailArea = activeTail.reduce(0, +) / (abs(range) <= Double.ulpOfOne ? 1 : range)
        let rising = values.contains { $0 < maximum * 0.7 }
        result.rejected = !((result.position != 0 || result.value != 0) && rising
            && result.position - 20 <= levelPosition
            && (activeTail.count > 19 || tailArea >= 10)
            && !(relativeSlope < 0.0015 && relativeVariance < 0.1)
            && (baselineRatio <= 0.35 || contrast >= 0.65))
        return result
    }

    static func select(all values: [Double], positions: [[Double]]) -> Selection {
        var result = all(values)
        let maximum = max(0, values.max() ?? 0)
        let candidates = positions.map { each($0, allMaximum: maximum) }
        let later = candidates.filter { !$0.rejected && $0.position > 0 }.map(\.position).max() ?? 0
        if result.position <= later - 35 { result.position = later }
        return result
    }

    private static func sumEight(_ values: [Double], squaredDistanceFrom mean: Double? = nil) -> Double {
        let count = values.count / 8 * 8
        var lanes = [Double](repeating: 0, count: 8)
        for index in 0..<count {
            if let mean {
                let difference = values[index] - mean
                lanes[index % 8] = lanes[index % 8].addingProduct(difference, difference)
            } else { lanes[index % 8] += values[index] }
        }
        var sum = lanes[6] + lanes[4] + lanes[2] + lanes[0] + lanes[7] + lanes[5] + lanes[3] + lanes[1]
        for index in count..<values.count {
            if let mean {
                let difference = values[index] - mean
                sum = sum.addingProduct(difference, difference)
            } else { sum += values[index] }
        }
        return sum
    }
}
