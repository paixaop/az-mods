#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativePhraseTiming {
    struct Prepared {
        let timing: NativePhraseFeatures.Timing
        let genreSnares: NativePhraseSnareTiming.Positions
        let rhythmType: Int
        let initialBoundary: Int
    }

    static func prepare(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                        fixedGrid: Bool) throws -> Prepared {
        let rate = samples.sourceRate / 8
        let input = Array(samples.middle.prefix(Int(rate * 210)))
        let bpm = timing.bpm, offset = timing.offsetMilliseconds
        func coordinates(_ offset: Int) -> [Int] {
            NativePhraseFeatures.stageBCoordinates(sampleCount: input.count, rate: rate,
                bpm: bpm, offsetMilliseconds: offset)
        }
        var positions = coordinates(offset)
        var attack = NativePhraseRhythm.Attacks(rate: rate, second: false)
        let rhythm = attack.processTiming(input, rate: rate, bpm: bpm, offsetMilliseconds: offset)
        var low = NativePhraseRhythm.PowerProcessor(rate: rate, low: true)
        let powers = low.process(samples.middle, rate: rate, bpm: bpm, coordinates: positions, beatCount: positions.count)
        let flags = try NativePhraseClassifier.rhythmFlags(rhythm: rhythm, bpm: bpm, blockPowers: powers.bars)
        let filtered = NativePhraseSnareTiming.filter(input, sampleRate: rate)
        let snare = NativePhraseSnareTiming.detect(filtered: filtered, sampleRate: rate, bpm: bpm,
            offsetMilliseconds: offset, beatCoordinates: positions, count: positions.count)
        let genre = NativePhraseSnareTiming.forGenre(windows: snare.windows, sampleRate: rate, bpm: bpm,
            offsetMilliseconds: offset, count: positions.count, rhythm: rhythm, fourKick: flags.fourKick)
        let bar = NativePhraseSnareTiming.forBar(windows: snare.windows, sampleRate: rate, bpm: bpm,
            offsetMilliseconds: offset, count: positions.count, rhythm: rhythm)
        let correction = correctOffset(samples: input, rate: rate, bpm: bpm, offset: offset,
            rhythm: rhythm, beatCount: positions.count, snares: snare.positions.values,
            snareCount: snare.positions.count, firstSnare: snare.positions.first)
        let corrected = fixedGrid ? offset : correction.offset
        positions = coordinates(corrected)
        let phase: Int
        if fixedGrid || bar.count < 1 || correction.strongAlignedBeat != 0 { phase = timing.phase }
        else {
            phase = correctBar(samples: input, rate: rate, bpm: bpm, originalOffset: offset,
                correctedOffset: corrected, initialPhase: 1, coordinates: positions, snares: bar.values,
                strengths: bar.strengths, count: bar.count, first: bar.first, suppliedPhase: timing.phase,
                correctionType: correction.type).phase
        }
        return Prepared(timing: .init(bpm: bpm, offsetMilliseconds: corrected, phase: phase),
            genreSnares: genre, rhythmType: correction.rhythmType,
            initialBoundary: NativePhraseFeatures.activityStart(samples: samples.middle, rate: rate,
                bpm: bpm, offsetMilliseconds: corrected))
    }
}
