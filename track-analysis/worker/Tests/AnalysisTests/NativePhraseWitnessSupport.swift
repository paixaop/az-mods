import Foundation
@testable import Analysis

// Test-only evidence export for executing the original binary on identical inputs.
enum NativePhraseWitnessSupport {
    static func dump(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                                  decision: NativePhraseInference.Decision, to url: URL) throws {
        let prepared = try NativePhraseTiming.prepare(samples: samples, timing: timing, fixedGrid: true)
        let beats = try NativePhraseFeatures.make(samples: samples, timing: prepared.timing)
        let rhythm = try NativePhraseRhythmFeatures.make(samples: samples, timing: prepared.timing, coordinates: beats.coordinates)
        let cqt = try NativePhraseOpening.makeSpectra(samples: samples, timing: prepared.timing, coordinates: beats.coordinates)
        let vocalFFT = try NativePhraseSpectra.make(samples: samples, timing: prepared.timing, coordinates: beats.coordinates, recurrence: false)
        let recurrenceFFT = try NativePhraseSpectra.make(samples: samples, timing: prepared.timing, coordinates: beats.coordinates, recurrence: true)
        let firstFrame = NativePhraseOpening.firstSpectralFrame(sampleRate: samples.sourceRate / 8,
            bpm: timing.bpm, firstBeat: prepared.timing.phase, offsetMilliseconds: 0)
        let opening = NativePhraseOpening.spectralFeatures(spectra: cqt, frameCount: cqt.first?.count ?? 0,
            firstFrame: firstFrame, beatCount: beats.coordinates.beatCount)
        let vocal = try NativePhraseSpectra.vocalActivity(vocalFFT)
        var arrays = [String: Any]()
        func array<T>(_ offset: String, _ kind: String, _ values: [T]) { arrays[offset] = ["kind": kind, "values": values] }
        array("210", "i", prepared.genreSnares.values); array("230", "d", prepared.genreSnares.strengths)
        array("e50", "d", rhythm.lowPower); array("e70", "d", rhythm.allPower)
        array("e98", "d", rhythm.allPeaks); array("eb8", "d", beats.lowPeaks)
        array("ef8", "d", beats.middlePeaks); array("f18", "d", beats.lowPeakChanges)
        array("f38", "B", rhythm.hiHatCounts); array("f58", "B", rhythm.firstAttacks)
        array("f78", "d", beats.lowLevels); array("f98", "B", rhythm.secondAttacks)
        array("fb8", "d", beats.lowQuarterPeaks); array("fd8", "d", beats.middleLevels)
        array("ff8", "d", rhythm.dividedLow); array("1018", "d", rhythm.dividedMiddle)
        array("1070", "d", rhythm.subPower); array("10b0", "d", beats.allLevels)
        array("10d0", "d", beats.highEighthPeaks); array("10f8", "i", rhythm.finalMiddleCoordinates)
        var scalars = [String: Any]()
        func scalar<T>(_ offset: String, _ kind: String, _ value: T) { scalars[offset] = ["kind": kind, "value": value] }
        scalar("80", "d", samples.sourceRate); scalar("90", "d", samples.sourceRate / 8)
        scalar("a8", "d", timing.bpm); scalar("a4", "i", timing.offsetMilliseconds)
        scalar("b0", "i", 0); scalar("b8", "i", samples.sourceFrameCount)
        scalar("200", "i", prepared.timing.offsetMilliseconds); scalar("204", "i", prepared.timing.phase)
        scalar("208", "i", prepared.rhythmType); scalar("250", "i", prepared.genreSnares.count)
        scalar("254", "i", prepared.genreSnares.first); scalar("258", "i", prepared.initialBoundary)
        scalar("e90", "i", beats.coordinates.beatCount); scalar("1068", "i", beats.coordinates.beatCount)
        scalar("10a8", "i", cqt.first?.count ?? 0); scalar("10ac", "i", firstFrame)
        scalar("10f0", "i", rhythm.zeroDataFlag)
        let info: [String: Any] = ["arrays": arrays, "scalars": scalars,
            "matrices": ["ee0": vocalFFT, "1038": [[Double]](), "1050": recurrenceFFT, "1090": cqt],
            "phase": prepared.timing.phase, "duration": Double(samples.sourceFrameCount) / samples.sourceRate,
            "initialBoundary": beats.initialBoundary, "route": decision.route,
            "openingHypotheses": decision.opening.hypotheses, "opening": decision.opening.selected,
            "openingFlag": decision.opening.hasOpening ? 1 : 0,
            "openingEnvelope": opening.envelope, "vocalActivity": vocal, "hiHatChanges": rhythm.hiHatChanges,
            "lowActivity": beats.lowActivity.values, "middleActivity": beats.middleActivity.values,
            "boundaries": decision.boundaries,
            "output": try JSONSerialization.jsonObject(with: JSONEncoder().encode(decision.output.entries))]
        try JSONSerialization.data(withJSONObject: info, options: [.sortedKeys]).write(to: url, options: .atomic)
        for (name, values) in [("low", samples.low), ("middle", samples.middle), ("high", samples.high)] {
            try values.withUnsafeBytes { Data($0) }.write(to: url.deletingPathExtension().appendingPathExtension("\(name).f64"), options: .atomic)
        }
    }
}
