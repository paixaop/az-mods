#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Completes the variable-tempo model grid after local interval reconciliation.
enum NativeBeatFocusDynamicGrid {
    private struct Transition { var index: Int; let whole: Int }

    static func finish(beats input: [NativeBeatInfo], sampleRate: Int,
                       sourceFrameCount: Int, anchorSample: Int,
                       anchorBar: Int) throws -> NativeBeatFocus.Result {
        guard sampleRate > 0, input.count >= 4 else { throw NativeBeatFocus.Failure.invalidInput }
        var beats = input
        for _ in 0..<2 {
            try Task.checkCancellation()
            fillGaps(&beats)
            var changes = transitions(beats)
            for pair in 0..<(changes.count / 2) {
                try Task.checkCancellation()
                let a = changes[pair * 2], b = changes[pair * 2 + 1]
                reshapeForward(&beats, at: a.index, whole: a.whole)
                reshapeBackward(&beats, at: b.index, whole: b.whole)
                if a.whole == 4, b.whole == 3 || b.whole == 4 {
                    shiftBackward(&beats, first: a.index + 4, last: b.index)
                    let start = a.index + 4
                    beats[start].sampleIndex -= Int(Double(beats[start].sampleIndex - beats[start - 1].sampleIndex) / 3 + 0.5)
                    if b.whole == 4 {
                        let value = Int(Double(beats[b.index].sampleIndex + beats[b.index + 1].sampleIndex) * 0.5 + 0.5)
                        beats.insert(blank(value), at: b.index + 1)
                        for j in (pair * 2 + 2)..<changes.count { changes[j].index += 1 }
                    }
                } else if a.whole == 3, b.whole == 3 || b.whole == 4 {
                    let end = b.index - (b.whole == 3 ? 1 : 0)
                    shiftForward(&beats, first: a.index + 4, end: end)
                    if b.whole == 3 {
                        beats.remove(at: b.index)
                        beats[b.index - 1].sampleIndex += Int(Double(beats[b.index].sampleIndex - beats[b.index - 1].sampleIndex) / 3 + 0.5)
                        for j in (pair * 2 + 2)..<changes.count { changes[j].index -= 1 }
                    } else {
                        beats[b.index].sampleIndex += Int(Double(beats[b.index + 1].sampleIndex - beats[b.index].sampleIndex) / 3 + 0.5)
                    }
                }
            }
            if let change = changes.last, changes.count % 2 == 1 {
                reshapeForward(&beats, at: change.index, whole: change.whole)
                if change.whole == 4 {
                    let start = change.index + 4
                    shiftBackward(&beats, first: start, last: beats.count - 1)
                    beats[start].sampleIndex -= Int(Double(beats[start].sampleIndex - beats[start - 1].sampleIndex) / 3 + 0.5)
                    let value = beats[beats.count - 1].sampleIndex * 2 - beats[beats.count - 2].sampleIndex
                    if value <= sourceFrameCount { beats.append(blank(value)) }
                } else if change.whole == 3 {
                    shiftForward(&beats, first: change.index + 4, end: beats.count - 1)
                    let value = beats[beats.count - 1].sampleIndex * 2 - beats[beats.count - 2].sampleIndex
                    if value > sourceFrameCount { beats.removeLast() }
                    else { beats[beats.count - 1].sampleIndex = value }
                }
            }
        }
        guard beats.count >= 4 else { throw NativeBeatFocus.Failure.invalidReconciliation }
        var tempos: [Float] = []
        for i in 0..<(beats.count - 1) {
            let delta = beats[i + 1].sampleIndex - beats[i].sampleIndex
            guard delta != 0 else { throw NativeBeatFocus.Failure.invalidReconciliation }
            tempos.append((Float(sampleRate) * 60) / Float(delta))
        }
        tempos.append(tempos[tempos.count - 1])
        tempos.sort()
        let middle = tempos.count / 2
        let bpm = tempos.count % 2 == 1 ? tempos[middle] : (tempos[middle] + tempos[middle - 1]) * 0.5
        var nearest = 0
        while nearest + 1 < beats.count,
              abs(beats[nearest + 1].sampleIndex - anchorSample) < abs(beats[nearest].sampleIndex - anchorSample) {
            nearest += 1
        }
        var bar = anchorBar
        if beats.count > 4 {
            for i in 2..<(beats.count - 2) {
                if beats[i].sampleIndex / sampleRate / 60 != beats[i + 1].sampleIndex / sampleRate / 60 {
                    let continuous = ((i - 2)...(i + 1)).allSatisfy {
                        let delta = beats[$0 + 1].beatInBar - beats[$0].beatInBar
                        return delta == 1 || delta == -3
                    }
                    if !continuous { nearest = 0; bar = beats[0].beatInBar }
                }
            }
        }
        for i in beats.indices {
            let value = (bar - nearest - 1 + i) % 4
            beats[i].beatInBar = (value < 0 ? value + 4 : value) + 1
            let first = max(0, i - 8), last = min(beats.count - 1, i + 8)
            beats[i].periodMilliseconds = (Float(beats[last].sampleIndex - beats[first].sampleIndex) * 1000)
                / (Float(sampleRate) * Float(last - first))
        }
        let firstBar = beats.prefix(4).firstIndex(where: { $0.beatInBar == 1 }) ?? 0
        var lastGrouped = 0
        if firstBar < beats.count - 4 {
            var i = firstBar
            repeat {
                var sum: Float = 0
                for j in i..<(i + 4) { sum += beats[j].periodMilliseconds }
                let mean = sum / 4
                for j in i..<(i + 4) { beats[j].periodMilliseconds = mean }
                i += 4
            } while i < beats.count - 4
            lastGrouped = i
        }
        for i in 0..<firstBar { beats[i].periodMilliseconds = beats[firstBar].periodMilliseconds }
        if lastGrouped > 0, lastGrouped < beats.count {
            for i in lastGrouped..<beats.count { beats[i].periodMilliseconds = beats[lastGrouped - 1].periodMilliseconds }
        }
        return .init(beats: beats, bpm: bpm, changedTempoOctave: false)
    }

    private static func blank(_ position: Int) -> NativeBeatInfo {
        .init(sampleIndex: position, intervalInBeats: 0, periodMilliseconds: 0,
              beatInBar: 0, sourceBand: -1, flag: 0)
    }

    private static func fillGaps(_ beats: inout [NativeBeatInfo]) {
        let originalCount = beats.count
        guard originalCount > 2 else { return }
        for i in 1..<(originalCount - 1) {
            let previous = beats[i].sampleIndex - beats[i - 1].sampleIndex
            let gap = beats[i + 1].sampleIndex - beats[i].sampleIndex
            guard previous != 0 else { continue }
            let ratio = Double(gap) / Double(previous)
            guard ratio.isFinite, ratio > 1.8 else { continue }
            if i < originalCount - 2 {
                let following = beats[i + 2].sampleIndex - beats[i + 1].sampleIndex
                guard following != 0, Double(gap) / Double(following) > 1.8 else { continue }
            }
            let subdivisions = Int(ratio + 0.5)
            guard subdivisions > 1 else { continue }
            let interval = Int(Double(gap) / Double(subdivisions) + 0.5)
            let origin = beats[i].sampleIndex
            for j in 1..<subdivisions { beats.insert(blank(origin + interval * j), at: i + j) }
        }
    }

    private static func transitions(_ beats: [NativeBeatInfo]) -> [Transition] {
        guard beats.count >= 65 else { return [] }
        var result: [Transition] = [], elapsed = 0, active = false
        for i in 32..<(beats.count - 32) {
            let span = beats[i + 4].sampleIndex - beats[i].sampleIndex
            let previous = beats[i].sampleIndex - beats[i - 4].sampleIndex
            let next = beats[i + 8].sampleIndex - beats[i + 4].sampleIndex
            let before = previous == 0 ? 0 : Double(span) * 4 / Double(previous)
            let after = next == 0 ? 0 : Double(span) * 4 / Double(next)
            let fractionBefore = before - before.rounded(.towardZero)
            let fractionAfter = after - after.rounded(.towardZero)
            if fractionBefore > 0.4, fractionBefore < 0.6, fractionAfter > 0.4, fractionAfter < 0.6 {
                let matches = Int(before) == Int(after) && elapsed >= 5
                if matches, !active {
                    result.append(.init(index: i, whole: Int(before)))
                    if result.count == 128 { break }
                    elapsed = 0
                    active = true
                } else { active = matches }
            } else { active = false }
            elapsed += 1
        }
        return result
    }

    private static func reshapeForward(_ beats: inout [NativeBeatInfo], at index: Int, whole: Int) {
        let span = Double(beats[index + 4].sampleIndex - beats[index].sampleIndex)
        let reciprocal = 1 / (Double(whole) + 0.5)
        beats[index].sampleIndex = Int(Double(beats[index].sampleIndex) + 0.5)
        let origin = Double(beats[index].sampleIndex)
        beats[index + 1].sampleIndex = Int(0.5.addingProduct(reciprocal, span) + origin)
        beats[index + 2].sampleIndex = Int(0.5.addingProduct(reciprocal + reciprocal, span) + origin)
        beats[index + 3].sampleIndex = Int(0.5.addingProduct(reciprocal * span, 3) + origin)
    }

    private static func reshapeBackward(_ beats: inout [NativeBeatInfo], at index: Int, whole: Int) {
        let span = Double(beats[index + 4].sampleIndex - beats[index].sampleIndex)
        let reciprocal = 1 / (Double(whole) + 0.5)
        beats[index + 4].sampleIndex = Int(Double(beats[index + 4].sampleIndex) + 0.5)
        let origin = Double(beats[index + 4].sampleIndex)
        beats[index + 3].sampleIndex = Int(0.5.addingProduct(-reciprocal, span) + origin)
        beats[index + 2].sampleIndex = Int(0.5.addingProduct(reciprocal * span, -2) + origin)
        beats[index + 1].sampleIndex = Int(0.5.addingProduct(reciprocal * span, -3) + origin)
    }

    private static func shiftBackward(_ beats: inout [NativeBeatInfo], first: Int, last: Int) {
        if first < last {
            for i in stride(from: last, through: first + 1, by: -1) {
                beats[i].sampleIndex = Int(Double(beats[i - 1].sampleIndex + beats[i].sampleIndex) * 0.5 + 0.5)
            }
        }
    }

    private static func shiftForward(_ beats: inout [NativeBeatInfo], first: Int, end: Int) {
        if first < end {
            for i in first..<end {
                beats[i].sampleIndex = Int(Double(beats[i].sampleIndex + beats[i + 1].sampleIndex) * 0.5 + 0.5)
            }
        }
    }
}
