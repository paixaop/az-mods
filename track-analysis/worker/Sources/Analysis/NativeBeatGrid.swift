#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Converts detector samples to the original encoded-audio clock. Playback-clock
/// translation belongs at the public API boundary, after phrase classification.
enum NativeBeatGrid {
    struct SampleBeat: Codable, Equatable, Sendable {
        var sampleIndex: Int32
        var beatInBar: Int32
        var bpm100: Int32
    }

    struct DetailedBeat: Codable, Equatable, Sendable {
        var beatInBar: UInt16
        var bpm100: UInt16
        var milliseconds: UInt32
        var microsecondRemainder: UInt16

        var microseconds: Int64 {
            Int64(milliseconds) * 1_000 + Int64(microsecondRemainder)
        }
    }

    struct FirstBeat: Codable, Equatable, Sendable {
        var offsetMilliseconds: Int64
        var phase: Int
    }

    struct Result: Codable, Equatable, Sendable {
        var beats: [DetailedBeat]
        var globalBPM100: Int
        var firstBeat: FirstBeat
    }

    enum InputError: Error {
        case invalidSampleRate, invalidSampleCount, invalidBeat, invalidTempo
        // The original keeps the initial count across repeated octave changes.
        // Some repeated halvings consequently read beyond their new allocation.
        case undefinedNativeNormalization
    }

    static let tempoRanges = [
        70...180, 70...180, 48...95, 58...115, 68...135, 78...155,
        88...175, 98...195, 108...215, 118...235, 128...255
    ]

    static func convert(samples: [SampleBeat], sampleRate: Int, sampleCount: Int,
                        globalBPM100: Int, rangeIndex: Int = 1, dynamic: Bool = false) throws -> Result {
        guard sampleRate > 0, sampleRate <= Int(Int32.max) else { throw InputError.invalidSampleRate }
        guard sampleCount >= 0, sampleCount <= Int(Int32.max) else { throw InputError.invalidSampleCount }
        guard globalBPM100 >= 0, globalBPM100 <= Int(Int32.max) else { throw InputError.invalidTempo }
        let original = try samples.map { sample in
            guard sample.sampleIndex >= 0, sample.beatInBar >= 0, sample.beatInBar <= Int32(UInt16.max),
                  sample.bpm100 >= 0, sample.bpm100 <= Int32(UInt16.max) else { throw InputError.invalidBeat }
            let microseconds = Int64(sample.sampleIndex) * 1_000_000 / Int64(sampleRate)
            guard let milliseconds = UInt32(exactly: microseconds / 1_000) else { throw InputError.invalidBeat }
            return DetailedBeat(beatInBar: UInt16(sample.beatInBar), bpm100: UInt16(sample.bpm100),
                milliseconds: milliseconds, microsecondRemainder: UInt16(microseconds % 1_000))
        }
        var result = original
        var adjustedBPM = globalBPM100
        if !dynamic, let first = samples.first {
            var effectiveBPM = globalBPM100 > 0 ? globalBPM100 : Int(first.bpm100)
            let bounds = tempoRanges[(1...10).contains(rangeIndex) ? rangeIndex : 1]
            let lower = bounds.lowerBound * 100, upper = bounds.upperBound * 100
            if effectiveBPM < lower || effectiveBPM > upper {
                guard effectiveBPM > 0 else { throw InputError.invalidTempo }
                let duration = durationMicroseconds(sampleRate: sampleRate, sampleCount: sampleCount)
                var current = original
                var accumulated = [DetailedBeat]()
                while effectiveBPM < lower {
                    guard current.count >= original.count else { throw InputError.undefinedNativeNormalization }
                    accumulated = doubled(Array(current.prefix(original.count)), durationMicroseconds: duration)
                    effectiveBPM *= 2
                    if effectiveBPM < lower { current = accumulated; accumulated = [] }
                }
                while effectiveBPM > upper {
                    guard current.count >= original.count else { throw InputError.undefinedNativeNormalization }
                    accumulated = halved(Array(current.prefix(original.count)), durationMicroseconds: duration, prefix: accumulated)
                    effectiveBPM /= 2
                    if effectiveBPM > upper { current = accumulated; accumulated = [] }
                }
                adjustedBPM = effectiveBPM
                result = accumulated
                if result.isEmpty { result = original }
            }
        }
        return Result(beats: result, globalBPM100: adjustedBPM,
            firstBeat: firstBeat(result, globalBPM100: adjustedBPM))
    }

    /// The endpoint calculation uses floating-point arithmetic in the original
    /// tempo-scaling helpers, unlike the integer sample-position conversion.
    static func durationMicroseconds(sampleRate: Int, sampleCount: Int) -> Int64 {
        Int64(Double(sampleCount) * 1_000_000 / Double(sampleRate))
    }

    static func firstBeat(_ beats: [DetailedBeat], globalBPM100: Int,
                          initial: FirstBeat = .init(offsetMilliseconds: -1, phase: -1)) -> FirstBeat {
        guard beats.count >= 8 else { return initial }
        let first = beats[0]
        let phaseMap = [1, 4, 3, 2]
        var phase = (1...4).contains(first.beatInBar) ? phaseMap[Int(first.beatInBar) - 1] : 1
        var offset = Int64(first.milliseconds)
        if Int(first.bpm100) != globalBPM100 {
            let period = 6_000_000 / Double(globalBPM100)
            if Double(offset) > period, Double(offset) < period + period {
                offset -= Int64(period)
                phase = phase == 1 ? 4 : phase - 1
            }
        }
        return FirstBeat(offsetMilliseconds: offset, phase: phase)
    }

    static func doubled(_ beats: [DetailedBeat], durationMicroseconds duration: Int64) -> [DetailedBeat] {
        guard let first = beats.first else { return [] }
        let firstPositions = [1, 3, 1, 3]
        let position = (1...4).contains(first.beatInBar) ? firstPositions[Int(first.beatInBar) - 1] : Int(first.beatInBar)
        var result = [DetailedBeat]()
        result.reserveCapacity(beats.count * 2)
        for index in beats.indices {
            var beat = beats[index]
            beat.bpm100 = beat.bpm100 &* 2
            result.append(beat)
            let interval: Int64
            if index + 1 < beats.count { interval = beats[index + 1].microseconds - beat.microseconds }
            else if index > 0 { interval = beat.microseconds - beats[index - 1].microseconds }
            else { interval = duration - beat.microseconds }
            let midpoint = beat.microseconds + interval / 2
            beat.milliseconds = UInt32(truncatingIfNeeded: midpoint / 1_000)
            beat.microsecondRemainder = UInt16(truncatingIfNeeded: midpoint % 1_000)
            result.append(beat)
        }
        while let last = result.last, last.microseconds > duration { result.removeLast() }
        return renumbered(result, firstPosition: position, durationMicroseconds: duration)
    }

    static func halved(_ beats: [DetailedBeat], durationMicroseconds duration: Int64,
                       prefix: [DetailedBeat] = []) -> [DetailedBeat] {
        guard let first = beats.first else { return [] }
        let firstPositions = [1, 4, 4, 1]
        let position = (1...4).contains(first.beatInBar) ? firstPositions[Int(first.beatInBar) - 1] : Int(first.beatInBar)
        let selected = stride(from: 0, to: beats.count, by: 2).map { index in
            var beat = beats[index]
            beat.bpm100 /= 2
            return beat
        }
        return renumbered(prefix + selected, firstPosition: position, durationMicroseconds: duration)
    }

    static func renumbered(_ beats: [DetailedBeat], firstPosition: Int,
                           durationMicroseconds duration: Int64) -> [DetailedBeat] {
        var result = beats
        var position = firstPosition - 1
        for index in result.indices {
            position = position % 4 + 1
            result[index].beatInBar = UInt16(truncatingIfNeeded: position)
        }
        if let first = result.first {
            let interval = result.count > 1 ? result[1].microseconds - first.microseconds : -first.microseconds
            if interval <= first.microseconds {
                let time = first.microseconds - interval
                let inserted = DetailedBeat(beatInBar: ((first.beatInBar &+ 2) & 3) + 1, bpm100: first.bpm100,
                    milliseconds: UInt32(truncatingIfNeeded: time / 1_000),
                    microsecondRemainder: UInt16(truncatingIfNeeded: time % 1_000))
                result.insert(inserted, at: 0)
            }
        } else {
            result.append(.init(beatInBar: 3, bpm100: 0, milliseconds: 0, microsecondRemainder: 0))
        }
        let last = result[result.count - 1], lastTime = last.microseconds
        guard duration >= lastTime else { return result }
        let interval = result.count > 1 ? lastTime - result[result.count - 2].microseconds : lastTime
        let remainder = duration - lastTime
        if remainder > 0, interval <= remainder {
            // The native representation intentionally retains the sum of the
            // microsecond remainders, even when that sum is at least 1,000.
            result.append(.init(beatInBar: (last.beatInBar & 3) + 1, bpm100: last.bpm100,
                milliseconds: last.milliseconds &+ UInt32(truncatingIfNeeded: interval / 1_000),
                microsecondRemainder: last.microsecondRemainder &+ UInt16(truncatingIfNeeded: interval % 1_000)))
        }
        return result
    }
}
