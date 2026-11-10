#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativeBeatDynamics {
    static func hasTimingDrift(positions: [Int], times: [Double], intervals: [Double],
                               referencePeriod: Double, sampleRate: Int, tempo: Float,
                               variableBPM: inout Float, constantBPM: inout Float) -> Bool {
        let count = positions.count
        guard count >= 2, referencePeriod > 0, tempo > 0 else { return true }
        let deviations = intervals.prefix(count - 1).map { $0 - referencePeriod }
        let runs = timingRuns(deviations, threshold: 10)
        let runCount = runs.reduce(0) { $0 + $1.upperBound - $1.lowerBound + 1 }
        let resizeRuns = Double(runCount) / Double(count) >= 0.5
            ? runs : timingRuns(deviations, threshold: 20)
        var resizedCount = 1
        for (index, range) in resizeRuns.enumerated() {
            let start = range.lowerBound, end = range.upperBound
            let first = start == 1 ? 0 : start - 2
            let last = min(end, count - 1)
            let missing = Int((((times[last] * 1000) - (times[first] * 1000)) * (1 / referencePeriod)).rounded(.toNearestOrAwayFromZero))
                - (end < count ? 1 : 0)
            if index == 0 {
                if start != 1 { resizedCount += start - 2 }
            } else { resizedCount += start - resizeRuns[index - 1].upperBound - 2 }
            if missing > 0 {
                if start != 1 { resizedCount += missing }
                if end + 1 < count { resizedCount += 1 }
            }
        }
        if let last = resizeRuns.last { resizedCount += max(0, count - last.upperBound - 1) }
        else { resizedCount += count - 1 }
        var samples = Array(positions.prefix(max(0, resizedCount)))
        if resizedCount > count { samples += Array(repeating: positions[count - 1], count: resizedCount - count) }

        let lower = Int(Double(count) * 0.05 + 0.5), upper = Int(Double(count) * 0.8 + 0.5)
        guard upper - lower >= 65 else { return true }
        let selectedRuns = runs.prefix { $0.lowerBound <= upper }
        var starts = [lower] + selectedRuns.map { $0.upperBound + 2 }
        var ends = selectedRuns.map { $0.lowerBound - 2 } + [upper]
        var index = 0
        while index < starts.count {
            if starts[index] > ends[index] { starts.remove(at: index); ends.remove(at: index) }
            else { index += 1 }
        }
        guard !starts.isEmpty else { return true }
        var period = Double(sampleRate) * 60 / Double(tempo)
        var residuals: [Int] = []
        for (start, end) in zip(starts, ends) {
            guard start >= 0, end < samples.count else { return true }
            let carried = residuals.last ?? 0, anchor = samples[start]
            residuals.append(carried)
            if start < end {
                for beat in (start + 1)...end {
                    let before = Int(0.5.addingProduct(Double(start), period))
                    let after = Int(0.5.addingProduct(Double(beat), period))
                    residuals.append(carried - anchor + before + samples[beat] - after)
                }
            }
        }
        guard residuals.count >= 2 else { return true }
        removeLinearTrend(&residuals, period: &period, sampleRate: sampleRate,
                          variableBPM: &variableBPM, constantBPM: &constantBPM)
        let changes = zip(residuals, residuals.dropFirst()).map { abs($1 - $0) }
        let smallestChange = changes.min() ?? 0
        var jumps = changes.enumerated().compactMap { Double($0.element - smallestChange) > period * 0.0625 ? $0.offset : nil }
        index = 0
        while index + 1 < jumps.count {
            let first = jumps[index], second = jumps[index + 1], distance = second - first
            if distance < 20 {
                residuals.removeSubrange((first + 1)...second)
                for next in (index + 1)..<jumps.count { jumps[next] -= distance }
                jumps.remove(at: index)
            } else { index += 1 }
        }
        guard residuals.count - jumps.count >= 2 else { return true }
        var minimum = residuals[0]
        if jumps.isEmpty {
            minimum = residuals.dropLast().min() ?? minimum
        } else {
            if jumps[0] > 6 { minimum = min(minimum, residuals[0..<(jumps[0] - 5)].min() ?? minimum) }
            if jumps.count > 1 {
                for part in 0..<(jumps.count - 1) where jumps[part] + 5 < jumps[part + 1] - 5 {
                    // The original middle-span reduction loads this single value.
                    minimum = min(minimum, residuals[part])
                }
            }
            let last = jumps[jumps.count - 1] + 5
            if last < residuals.count - 1 {
                minimum = min(minimum, residuals[last..<(residuals.count - 1)].min() ?? minimum)
            }
        }
        residuals = residuals.map { $0 - minimum }
        var signed: [Int] = [], absolute: [Int] = []
        if jumps.isEmpty {
            centerPhase(residuals, range: 0..<(residuals.count - 1), period: period,
                        middle: false, signed: &signed, absolute: &absolute)
        } else {
            if jumps[0] > 5 {
                centerPhase(residuals, range: 0..<(jumps[0] - 5), period: period,
                            middle: false, signed: &signed, absolute: &absolute)
            }
            if jumps.count > 1 {
                for part in 0..<(jumps.count - 1) {
                    let start = jumps[part] + 5, end = jumps[part + 1] - 5
                    if start < end {
                        centerPhase(residuals, range: start..<end, period: period,
                                    middle: true, signed: &signed, absolute: &absolute)
                    }
                }
            }
            let start = jumps[jumps.count - 1] + 5
            if start < residuals.count - 1 {
                centerPhase(residuals, range: start..<(residuals.count - 1), period: period,
                            middle: false, signed: &signed, absolute: &absolute)
            }
        }
        guard signed.count >= 64 else { return true }
        let meanCount = signed.count - 31
        let signedMeans = (0..<meanCount).map { index in Double(signed[index..<(index + 32)].reduce(0, +)) * 0.03125 }
        let absoluteMeans = (0..<meanCount).map { index in Double(absolute[index..<(index + 32)].reduce(0, +)) * 0.03125 }
        let amplitude = (absoluteMeans.max() ?? 0) - (absoluteMeans.min() ?? 0)
        let signedDeltas = (0..<(meanCount - 16)).map { signedMeans[$0 + 16] - signedMeans[$0] }
        let absoluteDeltas = (0..<(meanCount - 16)).map { absoluteMeans[$0 + 16] - absoluteMeans[$0] }
        let meanDelta = sum8(signedDeltas) / Double(signedDeltas.count)
        var varianceTerms = signedDeltas.map { ($0 - meanDelta) * ($0 - meanDelta) }
        let varianceVectorEnd = varianceTerms.count / 8 * 8
        var variance = sum8(Array(varianceTerms.prefix(varianceVectorEnd)))
        for index in varianceVectorEnd..<varianceTerms.count {
            let delta = signedDeltas[index] - meanDelta
            variance = variance.addingProduct(delta, delta)
        }
        varianceTerms.removeAll(keepingCapacity: false)
        let firstVariation = sum8(absoluteDeltas.map(abs))
        let secondDeltas = (0..<(absoluteDeltas.count - 16)).map { abs(absoluteDeltas[$0 + 16] - absoluteDeltas[$0]) }
        let secondVariation = sum8(secondDeltas)
        let originalCount = Double(count), firstCount = Double(signedDeltas.count), secondCount = Double(secondDeltas.count)
        return amplitude * originalCount > period * 0.13 * Double(meanCount)
            && (firstVariation * originalCount) / firstCount > firstCount * 380
            && (secondVariation * originalCount) / secondCount > secondCount * 130
            && (variance * originalCount) / firstCount > period * 6.4 * firstCount
    }

    private static func timingRuns(_ deviations: [Double], threshold: Double) -> [ClosedRange<Int>] {
        guard deviations.count >= 2 else { return [] }
        var ranges: [ClosedRange<Int>] = [], index = 0
        while index < deviations.count - 1, ranges.count < 128 {
            if abs(deviations[index]) > threshold, abs(deviations[index + 1]) > threshold {
                let start = index + 2
                index += 1
                while index < deviations.count - 1 {
                    index += 1
                    if abs(deviations[index]) <= threshold { break }
                }
                index += 1
                ranges.append(start...index)
            } else { index += 1 }
        }
        return ranges
    }

    private static func removeLinearTrend(_ values: inout [Int], period: inout Double, sampleRate: Int,
                                          variableBPM: inout Float, constantBPM: inout Float) {
        let count = values.count, end = count / 8 * 8
        let x = values.indices.map(Double.init), y = values.map(Double.init)
        let meanX = sum8(x) / Double(count), meanY = sum8(y) / Double(count)
        var xx = sum8(Array(x.prefix(end)).map { $0 * $0 })
        var xy = sum8((0..<end).map { x[$0] * y[$0] })
        for index in end..<count {
            xx = xx.addingProduct(x[index], x[index])
            xy = xy.addingProduct(x[index], y[index])
        }
        let denominator = xx - (meanX * Double(count)) * meanX
        guard abs(denominator) > Double.ulpOfOne else { return }
        let numerator = xy.addingProduct(-(meanX * Double(count)), meanY)
        let slope = numerator / denominator
        for index in values.indices {
            let centered = x[index] - meanX
            let predicted = meanY.addingProduct(slope, centered)
            values[index] -= Int(predicted.rounded(.toNearestOrAwayFromZero))
        }
        guard abs(period - slope) > Double.ulpOfOne else { return }
        let rate = Double(sampleRate) * 60
        let old = rate / period, new = rate / (period - slope)
        let roundedOld = old.rounded(.toNearestOrAwayFromZero), roundedNew = new.rounded(.toNearestOrAwayFromZero)
        if roundedOld == roundedNew, abs(old - roundedOld) > abs(new - roundedNew) { period -= slope }
        let secondsCorrection = slope / Double(sampleRate)
        func refine(_ bpm: Float) -> Float {
            let seconds = 60 / Double(bpm)
            let proposed: Float = abs(seconds - secondsCorrection) > Double.ulpOfOne
                ? Float(60 / (seconds - secondsCorrection)) : 0
            let rounded = bpm.rounded(.toNearestOrAwayFromZero)
            if rounded == proposed.rounded(.toNearestOrAwayFromZero),
               abs(bpm - rounded) > abs(proposed - proposed.rounded(.toNearestOrAwayFromZero)) { return proposed }
            return bpm
        }
        variableBPM = refine(variableBPM); constantBPM = refine(constantBPM)
        let scaledRate = Double(sampleRate) * 600, scaledTempo = scaledRate / period
        let integerTempo = scaledTempo.rounded(.toNearestOrAwayFromZero)
        if scaledTempo <= integerTempo * 1.001, scaledTempo >= integerTempo * 0.999, integerTempo != 0 {
            period = scaledRate / integerTempo
        }
        func snap(_ bpm: Float) -> Float {
            let tenth = (bpm * 10).rounded(.toNearestOrAwayFromZero)
            return bpm <= tenth * Float(bitPattern: 0x3dcd013b)
                && bpm >= tenth * Float(bitPattern: 0x3dcc985f) ? tenth * 0.1 : bpm
        }
        variableBPM = snap(variableBPM); constantBPM = snap(constantBPM)
    }

    private static func centerPhase(_ values: [Int], range: Range<Int>, period: Double, middle: Bool,
                                    signed: inout [Int], absolute: inout [Int]) {
        let total = sum8(values[range].map(Double.init))
        let mean = middle ? (total / Double(range.count)) * (1 / period) : total / (period * Double(range.count))
        let whole = Double(Int(mean)), fraction = mean - whole
        let offset: Double?
        if -0.1 < fraction, fraction < 0.1 { offset = whole * period }
        else if fraction > 0.9 { offset = period.addingProduct(period, whole) }
        else if fraction < -0.9 { offset = (-period).addingProduct(whole, period) }
        else if 0.4 < fraction, fraction < 0.6 { offset = (whole + 0.5) * period }
        else if -0.6 < fraction, fraction < -0.4 { offset = (whole - 0.5) * period }
        else if 0.15 < fraction, fraction < 0.35 { offset = (whole + 0.25) * period }
        else if -0.35 < fraction, fraction < -0.15 { offset = (whole - 0.25) * period }
        else if 0.65 < fraction, fraction < 0.85 { offset = (whole + 0.75) * period }
        else if -0.85 < fraction, fraction < -0.65 { offset = (whole - 0.75) * period }
        else { offset = nil }
        for value in values[range] {
            if let offset {
                signed.append(Int((0.5 - offset) + Double(value)))
                absolute.append(Int(abs(Double(value) - offset) + 0.5))
            } else { signed.append(value); absolute.append(value) }
        }
    }
}
