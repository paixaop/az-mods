#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Conversion after the downbeat HMM, including the native sample-clock
/// extension and median tempo estimate consumed by BALib's focus stage.
enum NativeBeatExport {
    struct Event: Codable, Equatable, Sendable {
        var seconds: Float
        var beatInBar: Int
    }
    struct Beat: Codable, Equatable, Sendable {
        var index: Int
        var bpm: Int
        var pos: Int
        var msec: Int
        var numBar: Int
    }
    struct Result: Codable, Equatable, Sendable {
        var beats: [Beat]
        var globalBPM100: Int
    }
    enum Failure: Error { case invalidClock, unorderedEvents }

    static func make(events: [Event], sampleCount: Int, sampleRate: Int) throws -> Result {
        guard sampleRate > 0, sampleCount >= 0 else { throw Failure.invalidClock }
        guard events.allSatisfy({ $0.seconds.isFinite && $0.seconds >= 0 && (1...4).contains($0.beatInBar) }),
              zip(events, events.dropFirst()).allSatisfy({ $0.seconds < $1.seconds }) else {
            throw Failure.unorderedEvents
        }
        var values = events
        if values.count > 1 {
            let step = values[0].seconds - values[1].seconds
            var seconds = values[0].seconds + step
            var bar = values[0].beatInBar
            var prefix: [Event] = []
            while seconds >= 0 {
                bar = bar > 1 ? bar - 1 : 4
                prefix.append(Event(seconds: seconds, beatInBar: bar))
                seconds += step
            }
            values.insert(contentsOf: prefix.reversed(), at: 0)
        }
        var beats: [Beat] = []
        for (index, value) in values.enumerated() {
            let milliseconds = Int((value.seconds * 1_000).rounded(.toNearestOrAwayFromZero))
            var tempo: Float = 0
            if index > 0 {
                let delta = milliseconds - beats[index - 1].msec
                guard delta > 0 else { throw Failure.unorderedEvents }
                tempo = 60_000 / Float(delta)
                beats[index - 1].bpm = Int(tempo * 100)
                if index == values.count - 1 { tempo = Float(beats[index - 1].bpm) }
            }
            let position = Int(Float(milliseconds * sampleRate) * 0.001)
            beats.append(Beat(index: index, bpm: Int(tempo), pos: position,
                              msec: milliseconds, numBar: value.beatInBar))
        }
        if beats.count > 1 {
            let last = beats[beats.count - 1]
            let step = last.pos - beats[beats.count - 2].pos
            var position = last.pos + step, bar = last.numBar
            while position < sampleCount {
                bar = bar % 4 + 1
                // The native tail stores this auxiliary clock in hundredths;
                // the actual beat position remains an integer source sample.
                let milliseconds = Int((Double(position) * 100 * (1 / Double(sampleRate)))
                    .rounded(.toNearestOrAwayFromZero))
                beats.append(Beat(index: beats.count, bpm: last.bpm, pos: position,
                                  msec: milliseconds, numBar: bar))
                position += step
            }
        }
        let tempos = beats.map(\.bpm).sorted()
        let median: Int
        if tempos.isEmpty { median = 0 }
        else if tempos.count % 2 == 1 { median = tempos[tempos.count / 2] }
        else { median = Int((Double(tempos[tempos.count / 2 - 1]) + Double(tempos[tempos.count / 2])) * 0.5) }
        return Result(beats: beats, globalBPM100: median)
    }
}
