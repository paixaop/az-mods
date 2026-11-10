#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativeBeatAdjust {
    enum Failure: Error, Equatable { case noTempoEvidence }

    private struct TempoFit {
        var bpm = 0.0
        var score = 0.0
        var grid: [Int] = []
    }

    func analyze(tempo: inout Float, beats: inout [NativeBeatInfo]) throws {
        guard tempo > 0, !beats.isEmpty else { return }
        var count = beats.count
        if capacity >= sampleRate * 900 {
            count = Int((Double(tempo) * 0.016666666666666666
                * Double(sampleRate * 900).addingProduct(Double(beats[0].sampleIndex), -0.25)) / Double(sampleRate))
        }
        try adjustTempo(tempo: &tempo, beats: &beats, measuredCount: count)
        adjustPosition(beats: &beats)
    }

    private func adjustTempo(tempo: inout Float, beats: inout [NativeBeatInfo], measuredCount: Int) throws {
        guard measuredCount > 0, tempo > 0.05, !beats.isEmpty else { return }
        let first = (beats.first(where: { $0.sampleIndex >= -3 }) ?? beats.last!).sampleIndex / 4
        guard first >= 0, first < capacity else { return }
        var selected = try searchTempo(around: Double(tempo), first: first,
            count: beats.count, measuredCount: measuredCount)
        // With no positive envelope score, the original starts its alternate
        // search at a negative tempo and never clears the continuation flag.
        // Report that unsupported state instead of clamping zero to 70 BPM.
        guard selected.score > 0, selected.bpm > 0 else { throw Failure.noTempoEvidence }
        let halfWindow = abs(selected.bpm) > Double.ulpOfOne
            ? Int((Double(sampleRate) * 30) / selected.bpm + 0.5) : 0
        let shiftedFirst = ((beats.first(where: { $0.sampleIndex + halfWindow >= -3 }) ?? beats.last!).sampleIndex
            + halfWindow) / 4
        if shiftedFirst >= 0, shiftedFirst < capacity {
            let alternate = try searchTempo(around: selected.bpm, first: shiftedFirst,
                count: beats.count, measuredCount: measuredCount)
            if selected.score < alternate.score * 0.96 {
                selected = alternate
                for index in beats.indices { beats[index].sampleIndex += halfWindow }
            }
        }
        Self.reflect(selected, tempo: &tempo, beats: &beats)
    }

    private func searchTempo(around bpm: Double, first: Int, count: Int, measuredCount: Int) throws -> TempoFit {
        func search(direction: Double) throws -> (best: TempoFit, last: TempoFit) {
            var candidate = bpm - 0.05
            var best = TempoFit(), last = TempoFit()
            while true {
                try Task.checkCancellation()
                let trial = try peakTempo(start: candidate, first: first, count: count, measuredCount: measuredCount)
                last = trial.fit
                if best.score < trial.fit.score { best = trial.fit }
                if !trial.continueSearch { return (best, last) }
                candidate += direction * 0.05
            }
        }
        let fraction = bpm - Double(Int(bpm))
        let primary = try search(direction: fraction < 0.5 ? -1 : 1)
        var best = primary.best
        if fraction == 0.5 {
            let lower = try search(direction: -1)
            if best.score < lower.best.score { best = lower.best }
        }
        let last = primary.last.bpm, integer = Double(Int(last))
        if last != integer, integer * 0.9995 <= last, last <= integer * 1.0005 {
            let rounded = try peakTempo(start: integer - 0.05, first: first, count: count, measuredCount: measuredCount)
            if best.score < rounded.fit.score { best = rounded.fit }
        }
        return best
    }

    private func peakTempo(start: Double, first: Int, count: Int,
                           measuredCount: Int) throws -> (fit: TempoFit, continueSearch: Bool) {
        var scores: [Double] = [], bpms: [Double] = []
        scores.reserveCapacity(1632); bpms.reserveCapacity(1632)
        var best = TempoFit()
        for index in 0..<1632 {
            if index % 32 == 0 { try Task.checkCancellation() }
            let bpm = min(180, max(70, start.addingProduct(Double(index), 0.0000625)))
            let grid = makeGrid(bpm: bpm, first: first, count: count)
            let score = windowPeak(grid: grid, count: measuredCount)
            bpms.append(bpm); scores.append(score)
            if best.score < score { best = TempoFit(bpm: bpm, score: score, grid: grid) }
        }
        let peaks = NativeBeatPeaks.find(scores)
        var continuing = true
        if peaks.count >= 21,
           peaks[10..<(peaks.count - 10)].contains(where: { best.score * 0.075 < $0.prominence }) {
            continuing = false
            var greatestProminence = 0.0, selectedIndex = 0
            for peak in peaks where greatestProminence < peak.prominence {
                greatestProminence = peak.prominence; selectedIndex = peak.position - 1
            }
            best.bpm = bpms[selectedIndex]
            let strong = peaks.filter { best.score * 0.75 < $0.height }.count
            if Double(strong) / Double(peaks.count) > 0.5 { continuing = true }
        }
        let integer = Double(Int(start + 0.05))
        if start <= integer, integer <= start + 0.1 { continuing = false }
        if !(70.007 < best.bpm && best.bpm < 179.982) { continuing = false }
        return (best, continuing)
    }

    private func makeGrid(bpm: Double, first: Int, count: Int) -> [Int] {
        guard bpm > 0, first >= 0, first < capacity, count > 0 else { return [] }
        let period = (Double(sampleRate) * 60) / bpm
        let extent = Double(sourceFrameCount / 4)
        var grid: [Int] = []
        grid.reserveCapacity(count + 1)
        var position = 0.0
        for index in 0..<count {
            position = Double(first).addingProduct(Double(index), period)
            if position < extent { grid.append(Int(position + 0.5)) }
        }
        position += period
        while position < extent {
            grid.append(Int(position + 0.5)); position += period
        }
        return grid
    }

    private func windowPeak(grid: [Int], count: Int) -> Double {
        let half = Int(Double(sampleRate) * 0.05), width = half * 2
        let count = min(count, grid.count)
        guard width > 0, count > 2 else { return 0 }
        var sum = [Double](repeating: 0, count: width)
        for beat in 1..<(count - 1) {
            let start = grid[beat] - half, end = grid[beat] + half
            guard start >= 0, start < end, end <= capacity, end <= high.count else { continue }
            for index in 0..<width { sum[index] += Double(high[start + index]) }
        }
        return max(0, sum.max() ?? 0)
    }

    private static func reflect(_ selected: TempoFit, tempo: inout Float, beats: inout [NativeBeatInfo]) {
        guard selected.bpm > 0, tempo > 0, !beats.isEmpty, !selected.grid.isEmpty,
              Int(Double(tempo) * 1000) != Int(selected.bpm * 1000) else { return }
        if selected.grid.count < beats.count { beats.removeLast(beats.count - selected.grid.count) }
        while beats.count < selected.grid.count {
            var added = beats.last!
            added.beatInBar = added.beatInBar == 4 ? 1 : added.beatInBar + 1
            added.flag = 0
            beats.append(added)
        }
        var meter = beats[0].beatInBar, phase = meter - 1
        var distance = abs(beats[0].sampleIndex - selected.grid[0] * 4)
        for beat in beats.dropFirst() {
            meter = max(meter, beat.beatInBar)
            let nextDistance = abs(beat.sampleIndex - selected.grid[0] * 4)
            if nextDistance < distance { distance = nextDistance; phase = beat.beatInBar - 1 }
        }
        for index in beats.indices {
            beats[index].sampleIndex = selected.grid[index] * 4
            beats[index].periodMilliseconds = Float(60000 / selected.bpm)
            beats[index].beatInBar = meter == 0 ? phase + index + 1 : (phase + index) % meter + 1
        }
        tempo = Float(selected.bpm)
    }

    private func adjustPosition(beats: inout [NativeBeatInfo]) {
        guard !beats.isEmpty else { return }
        let half = Int(Double(sampleRate) * 0.05), width = half * 2
        var all = [Double](repeating: 0, count: width)
        var positions = [[Double]](repeating: all, count: 4)
        if beats.count >= 3 {
            for beat in 1..<(beats.count - 1) {
                let start = beats[beat].sampleIndex / 4 - half, end = start + width
                guard start >= 0, end <= capacity, end <= high.count else { continue }
                let group = (beat + 3) % 4
                for index in 0..<width {
                    let value = Double(high[start + index])
                    all[index] += value; positions[group][index] += value
                }
            }
        }
        let selected = NativeBeatAlignment.select(all: all, positions: positions)
        offsetPosition(half: half, selected: selected.position, beats: &beats)
    }

    private func offsetPosition(half: Int, selected: Int, beats: inout [NativeBeatInfo]) {
        guard !beats.isEmpty else { return }
        let period = Int(Double(Float(sampleRate) * beats[0].periodMilliseconds) * 0.004)
        guard period > 0 else { return }
        let offset = (selected - half) * 4
        var before = 0, after = 0
        for index in beats.indices {
            beats[index].sampleIndex += offset
            if beats[index].sampleIndex < 0 { before += 1 }
            if sourceFrameCount < beats[index].sampleIndex + offset { after += 1 }
        }
        beats.removeFirst(min(before, beats.count))
        beats.removeLast(min(after, beats.count))
        guard !beats.isEmpty else { return }
        while beats[0].sampleIndex >= period {
            var added = beats[0]
            added.sampleIndex -= period
            added.beatInBar = added.beatInBar == 1 ? 4 : added.beatInBar - 1
            added.flag = 0
            beats.insert(added, at: 0)
        }
        while let last = beats.last, last.sampleIndex + period <= sourceFrameCount {
            var added = last
            added.sampleIndex += period
            added.beatInBar = added.beatInBar == 4 ? 1 : added.beatInBar + 1
            added.flag = 0
            beats.append(added)
        }
    }
}
