#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseSnareTiming {
    struct Window {
        let start: Int
        let values: [Double]
        let indices: [Int]
    }
    struct Positions {
        var values: [Int]
        var strengths: [Double]
        let count: Int
        let first: Int
    }
    struct Detection {
        let positions: Positions
        let windows: [Window]
    }

    static func filter(_ samples: [Double], sampleRate: Double) -> [Double] {
        let rate = sampleRate == 0 ? 5512.5 : sampleRate
        var output = samples
        for damping in [1.8477590650225735, 0.7653668647301797, 1.8477590650225735, 0.7653668647301797] {
            var section = NativePhraseEnvelopes.secondOrder(rate: rate, angularCutoff: 1884.9555921538758,
                damping: damping, highPass: true)
            output = section.process(output)
        }
        var smoother = NativePhraseEnvelopes.smoother(rate: rate, angularCutoff: 31.41592653589793)
        output = smoother.process(output.map(abs))
        if output.count > 1 {
            for i in stride(from: output.count - 1, through: 1, by: -1) { output[i] -= output[i - 1] }
        }
        if !output.isEmpty { output[0] = 0 }
        return output
    }

    static func detect(filtered: [Double], sampleRate: Double, bpm: Double, offsetMilliseconds: Int,
                       beatCoordinates: [Int], count: Int) -> Detection {
        let n = count / 8 * 8
        let period = (sampleRate == 0 ? 5512.5 : sampleRate) * 60 / (bpm == 0 ? 120 : bpm)
        let separation = Int(period * 0.25 + 0.5) - 20
        var windows = [Window](), positions = [Int](repeating: 0, count: n), filled = 0
        for block in 0..<(count / 8) {
            let start = beatCoordinates[block * 4], end = beatCoordinates[(block + 1) * 4]
            precondition(start >= 1 && end > start && end - 1 <= filtered.count)
            let source = Array(filtered[(start - 1)..<(end - 1)])
            let order = source.indices.sorted { source[$0] == source[$1] ? $0 < $1 : source[$0] > source[$1] }
            let window = Window(start: start, values: order.map { source[$0] }, indices: order.map { $0 + 1 })
            windows.append(window)
            var accepted = 0
            guard source.count > 1 else { continue }
            for i in 0..<(source.count - 1) {
                if (0..<accepted).contains(where: { abs(window.indices[$0] - window.indices[i]) <= separation }) { continue }
                let current = window.values[i], next = window.values[i + 1]
                if next < current * 0.9886 && filled < n {
                    positions[filled] = start - 1 + window.indices[i]; filled += 1; accepted += 1
                } else if i + 2 >= source.count || filled >= n || next * 0.9886 <= window.values[i + 2] { break }
            }
        }
        positions.sort()
        let distance = Int(period + period + 0.5), tolerance = Int(period * 0.03125 + 0.5)
        for i in positions.indices where positions[i] != 0 {
            if !hasNeighbor(positions[i], in: positions, distance: distance, before: tolerance, after: tolerance) { positions[i] = 0 }
        }
        let zeroStrengths = [Double](repeating: 0, count: n)
        return Detection(positions: finish(positions: positions, strengths: zeroStrengths,
            offset: sampleOffset(milliseconds: offsetMilliseconds, rate: sampleRate)), windows: windows)
    }

    static func forGenre(windows: [Window], sampleRate: Double, bpm: Double, offsetMilliseconds: Int,
                         count: Int, rhythm: [UInt8], fourKick: Int) -> Positions {
        let period = (sampleRate == 0 ? 5512.5 : sampleRate) * 60 / (bpm == 0 ? 120 : bpm)
        let quarter = Int(period * 0.25 + 0.5), eighth = Int(period * 0.125 + 0.5)
        let offset = sampleOffset(milliseconds: offsetMilliseconds, rate: sampleRate)
        var candidates = collect(windows: windows, capacity: count / 8 * 8, separation: quarter - 20, factor: 0.9886, keepStrengths: true)
        sort(&candidates.positions, &candidates.strengths)
        var output = candidates
        for i in candidates.positions.indices where candidates.positions[i] != 0 {
            let position = candidates.positions[i]
            let twoBeat = hasNeighbor(position, in: candidates.positions, distance: Int(period + period + 0.5), before: eighth, after: eighth)
            let fourBeat = hasNeighbor(position, in: candidates.positions, distance: Int(period * 4 + 0.5), before: quarter, after: eighth)
            if twoBeat {
                let at = quarter == 0 ? 0 : (position - offset) / quarter
                if at > 5 && at + 5 < rhythm.count && !supportsSnare(rhythm, at: at) {
                    candidates.positions[i] = 0; candidates.strengths[i] = 0
                }
            } else {
                candidates.positions[i] = 0; candidates.strengths[i] = 0
                if !fourBeat { output.positions[i] = 0; output.strengths[i] = 0 }
            }
        }
        if fourKick != 1 {
            for i in output.positions.indices where output.positions[i] != 0 {
                let at = quarter == 0 ? 0 : (output.positions[i] - 1 - offset) / quarter
                if at >= 0 && at + 1 < rhythm.count && rhythm[at] == 1 { output.positions[i] = 0 }
            }
        }
        return finish(positions: output.positions, strengths: output.strengths, offset: offset)
    }

    static func forBar(windows: [Window], sampleRate: Double, bpm: Double, offsetMilliseconds: Int,
                       count: Int, rhythm: [UInt8]) -> Positions {
        let period = (sampleRate == 0 ? 5512.5 : sampleRate) * 60 / (bpm == 0 ? 120 : bpm)
        let quarter = Int(period * 0.25 + 0.5), eighth = Int(period * 0.125 + 0.5)
        let offset = sampleOffset(milliseconds: offsetMilliseconds, rate: sampleRate)
        var candidates = collect(windows: windows, capacity: count / 8 * 16, separation: quarter - 20,
            factor: 0.998849369936505, keepStrengths: false)
        sort(&candidates.positions, &candidates.strengths)
        for i in candidates.positions.indices where candidates.positions[i] != 0 {
            let position = candidates.positions[i]
            let neighbor = hasNeighbor(position, in: candidates.positions, distance: Int(period + period + 0.5), before: eighth, after: eighth)
            let at = quarter == 0 ? 0 : (position - offset) / quarter
            if !neighbor || (at >= 6 && at + 5 < rhythm.count && !supportsSnare(rhythm, at: at)) {
                candidates.positions[i] = 0; candidates.strengths[i] = 0
            }
        }
        return finish(positions: candidates.positions, strengths: candidates.strengths, offset: offset)
    }

    private static func collect(windows: [Window], capacity: Int, separation: Int, factor: Double,
                                keepStrengths: Bool) -> (positions: [Int], strengths: [Double]) {
        var positions = [Int](repeating: 0, count: capacity), strengths = [Double](repeating: 0, count: capacity), count = 0
        for window in windows where window.values.count > 1 {
            for i in 0..<(window.values.count - 1) {
                let position = window.start + window.indices[i]
                if (0..<count).contains(where: { abs(positions[$0] - position) <= separation }) { continue }
                if count < capacity {
                    let current = window.values[i], next = window.values[i + 1]
                    if next < current * factor {
                        positions[count] = position - 1
                        if keepStrengths { strengths[count] = current }
                        count += 1
                    } else if i + 2 >= window.values.count || next * factor <= window.values[i + 2] { break }
                }
            }
        }
        return (positions, strengths)
    }

    private static func hasNeighbor(_ position: Int, in positions: [Int], distance: Int, before: Int, after: Int) -> Bool {
        positions.contains { value in
            value != 0 && ((value - before <= position - distance && position - distance <= value + after)
                || (value - before <= position + distance && position + distance <= value + after))
        }
    }

    private static func supportsSnare(_ rhythm: [UInt8], at i: Int) -> Bool {
        ((rhythm[i - 4] == 1 && rhythm[i + 2] == 1)
            || (rhythm[i - 6] == 1 && rhythm[i + 5] == 1)
            || (rhythm[i - 4] == 1 && rhythm[i + 4] == 1)) && rhythm[i] != 1
    }

    private static func sampleOffset(milliseconds: Int, rate: Double) -> Int {
        Int(Double(milliseconds) * (rate == 0 ? 5512.5 : rate) * 0.001 + 0.5)
    }

    private static func sort(_ positions: inout [Int], _ strengths: inout [Double]) {
        let order = positions.indices.sorted { positions[$0] == positions[$1] ? $0 < $1 : positions[$0] < positions[$1] }
        positions = order.map { positions[$0] }; strengths = order.map { strengths[$0] }
    }

    private static func finish(positions: [Int], strengths: [Double], offset: Int) -> Positions {
        var p = positions, s = strengths
        sort(&p, &s)
        let first = p.firstIndex(where: { $0 != 0 }) ?? p.count
        for i in first..<p.count { p[i] -= offset }
        return Positions(values: p, strengths: s, count: p.count - first, first: first + 1)
    }
}
