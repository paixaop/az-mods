#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Peaks, prominence and interpolated half-prominence widths used by beat refinement.
enum NativeBeatPeaks {
    struct Peak: Equatable, Sendable {
        let position: Int
        let height: Double
        let prominence: Double
        let width: Double
    }

    static func find(_ values: [Double]) -> [Peak] {
        guard values.count >= 2 else { return [] }
        var positions: [Int] = []
        var previousSlope = 0
        for index in 0..<(values.count - 1) {
            let difference = values[index] - values[index + 1]
            let slope: Int
            if difference < 0 { slope = 1 }
            else {
                slope = difference == 0 ? 0 : 2
                if previousSlope == 1 { positions.append(index + 1) }
            }
            previousSlope = slope
        }
        return positions.map { position in
            let height = values[position - 1]
            var left = 1
            for candidate in stride(from: position, through: 1, by: -1) {
                if height < values[candidate - 1] { left = candidate; break }
            }
            var leftMinimum = values[left - 1]
            for index in left...position { leftMinimum = min(leftMinimum, values[index]) }
            var right = values.count
            for candidate in position...values.count {
                if height < values[candidate - 1] { right = candidate; break }
            }
            var rightMinimum = height
            for index in position...min(right, values.count - 1) {
                rightMinimum = min(rightMinimum, values[index])
            }
            let prominence = height - max(leftMinimum, rightMinimum)
            let level = height.addingProduct(prominence, -0.5)
            left = 1
            for candidate in stride(from: position, through: 1, by: -1) {
                if values[candidate - 1] < level { left = candidate; break }
            }
            let leftDelta = values[left] - values[left - 1]
            let leftFraction = leftDelta == 0 ? 0 : (level - values[left - 1]) / leftDelta
            right = values.count
            for candidate in position...values.count {
                if values[candidate - 1] < level { right = candidate; break }
            }
            let rightDelta = values[right - 1] - values[right - 2]
            let rightFraction = rightDelta == 0 ? 0 : (level - values[right - 2]) / rightDelta
            let width = (Double(right - 1) - (leftFraction + Double(left))) + rightFraction
            return Peak(position: position, height: height, prominence: prominence, width: width)
        }
    }
}
