#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

enum NativePhraseInference {
    struct Decision {
        let timing: NativePhraseFeatures.Timing
        let featureBeatCount: Int
        let tentativeMood: PhraseMood
        let mood: PhraseMood
        let route: Int
        let opening: NativePhraseOpening.Result
        let boundaries: [Int]
        let output: NativePhraseExport.Result
    }

    static func classify(samples: NativePhraseFeatures.Samples, timing inputTiming: NativePhraseFeatures.Timing,
                         fixedGrid: Bool, forcedMood: PhraseMood? = nil) throws -> Decision {
        try Task.checkCancellation()
        let prepared = try NativePhraseTiming.prepare(samples: samples, timing: inputTiming, fixedGrid: fixedGrid)
        let timing = prepared.timing
        let beats = try NativePhraseFeatures.make(samples: samples, timing: timing)
        let n = beats.coordinates.beatCount, phase = timing.phase
        let rhythm = try NativePhraseRhythmFeatures.make(samples: samples, timing: timing, coordinates: beats.coordinates)
        try Task.checkCancellation()
        let spectra = try NativePhraseOpening.makeSpectra(samples: samples, timing: timing, coordinates: beats.coordinates)
        let openingFeatures = NativePhraseOpening.spectralFeatures(spectra: spectra,
            frameCount: spectra.first?.count ?? 0,
            firstFrame: NativePhraseOpening.firstSpectralFrame(sampleRate: samples.sourceRate / 8,
                bpm: timing.bpm, firstBeat: phase, offsetMilliseconds: 0), beatCount: n)
        let vocalSpectra = try NativePhraseSpectra.make(samples: samples, timing: timing, coordinates: beats.coordinates, recurrence: false)
        let vocal = try NativePhraseSpectra.vocalActivity(vocalSpectra)
        let power = try NativePhraseClassifier.lowPower(low: rhythm.lowPower, full: rhythm.allPower, beatCount: n)
        let compression = try NativePhraseClassifier.compression(low: rhythm.lowPower, full: rhythm.allPower,
            rhythm: rhythm.firstAttacks, phase: phase)
        let slow = try NativePhraseClassifier.slowTempo(sampleRate: samples.sourceRate / 8, bpm: timing.bpm,
            blockPowers: power.blockPowers, onsetSamples: prepared.genreSnares.values,
            beatSamples: rhythm.finalMiddleCoordinates, onsetCount: prepared.genreSnares.count,
            firstOnset: prepared.genreSnares.first, sourceOffsetMS: inputTiming.offsetMilliseconds,
            analysisOffsetMS: timing.offsetMilliseconds)
        let flags = try NativePhraseClassifier.rhythmFlags(rhythm: rhythm.firstAttacks, bpm: timing.bpm, blockPowers: power.blockPowers)
        let bpmFlags = NativePhraseClassifier.bpmFlags(timing.bpm)
        var inputs = NativePhraseClassifier.MoodInputs()
        inputs.slowRock = try NativePhraseClassifier.slowRock(low: rhythm.firstAttacks, middle: rhythm.secondAttacks)
        inputs.slowTempo = slow.slow; inputs.lowPowerRatio = power.ratio
        inputs.popRock = flags.popRock; inputs.alternateRock = flags.alternateRock
        inputs.sparseKick = flags.sparseKick; inputs.fourKick = flags.fourKick
        inputs.fadeOut = NativePhraseMood.fadeOut(low: rhythm.lowPower, full: rhythm.allPower)
        inputs.narrowBPM = bpmFlags.narrow; inputs.broadBPM = bpmFlags.broad
        inputs.typicalGenre = NativePhraseClassifier.typicalGenre(bpm: timing.bpm, rhythmType: prepared.rhythmType)
        inputs.compression = compression.second
        let tentative = NativePhraseClassifier.initialMood(inputs)
        var mood = forcedMood ?? NativePhraseMood.reviseMid(tentative, beatCount: n,
            lowPeaks: beats.lowPeaks, quarterPeaks: beats.lowQuarterPeaks)
        var route = mood == .high ? 1 : 2
        let anchors = NativePhraseBoundaries.securePoints(lowPeaks: beats.lowPeaks, phase: phase)
        if mood == .mid {
            let alternatives = NativePhraseBoundaries.initialAlternatives(anchors: anchors, phase: phase)
            var loopTypes = [Int](), rhythmTypes = [Int]()
            if !alternatives.short.isEmpty {
                let positions = alternatives.short.map(\.beat), roles = alternatives.short.map(\.subtype)
                let blocks = NativePhraseHighClassifier.blockMeans(boundaries: positions, intro: openingFeatures.envelope,
                    low: beats.lowPeaks, quarterPower: beats.lowQuarterPeaks, phase: phase)
                loopTypes = NativePhraseDecorators.simpleLoops(boundaries: positions, roles: roles,
                    firstLevel: rhythm.dividedLow, secondLevel: rhythm.dividedMiddle, phraseMetric: blocks.low).roles
                rhythmTypes = NativePhraseDecorators.rhythm(boundaries: alternatives.long.map(\.beat),
                    roles: alternatives.long.map(\.subtype), activity: rhythm.secondAttacks).roles
            }
            let selected = try NativePhraseRoute.classify(lowPeaks: beats.lowPeaks, phase: phase,
                duration: Double(samples.sourceFrameCount) / samples.sourceRate,
                loopTypes: loopTypes, rhythmTypes: rhythmTypes, mood: mood)
            mood = forcedMood ?? selected.mood; route = selected.route
        }
        let middle = NativePhraseFeatures.middlePeakActivity(beats.middlePeaks, normalizationCount: n, phase: phase)
        let opening = try NativePhraseOpening.main(envelope: beats.lowPeaks, low: beats.lowActivity.values,
            middle: middle.bars, vocal: vocal, phase: phase,
            anchors: anchors.openingPositions.isEmpty ? [0] : anchors.openingPositions)
        let boundaries = NativePhraseBoundaries.candidates(beats: beats, openingHypotheses: opening.hypotheses,
            introEnd: opening.selected, openingFlag: opening.hasOpening ? 1 : 0,
            openingEnvelope: openingFeatures.envelope, vocalActivity: vocal,
            hihatChanges: rhythm.hiHatChanges, rhythm: rhythm.firstAttacks,
            durationSeconds: Double(samples.sourceFrameCount) / samples.sourceRate, route: route)
        try Task.checkCancellation()
        let output: NativePhraseExport.Result
        if mood == .high {
            let blocks = NativePhraseHighClassifier.blockMeans(boundaries: boundaries, intro: openingFeatures.envelope,
                low: beats.lowPeaks, quarterPower: beats.lowQuarterPeaks, phase: phase)
            let roles = NativePhraseHighClassifier.classify(boundaries: boundaries, introMeans: blocks.intro, lowMeans: blocks.low)
            let profile = NativePhraseFills.Profile(low: beats.lowPeaks, lowQuarterMean: beats.lowQuarterMean,
                highEighths: beats.highEighthPeaks,
                powerQuarters: rhythm.allPower, phase: phase, lowQuarters: beats.lowQuarterPeaks)
            let decorated = NativePhraseDecorators.high(boundaries: boundaries, roles: roles.roles, profile: profile,
                firstLevel: rhythm.dividedLow, secondLevel: rhythm.dividedMiddle, phraseMetric: blocks.low,
                activity: rhythm.secondAttacks)
            output = NativePhraseExport.convert(mood: mood, decorated: decorated,
                variation: .init(flag: roles.simpleAlternation ? 0 : 1, summary: 0), converterParameter: 0)
        } else {
            let recurrence = try NativePhraseSpectra.make(samples: samples, timing: timing, coordinates: beats.coordinates, recurrence: true)
            output = try NativePhraseExport.nonHigh(mood: mood, boundaries: boundaries, recurrenceSpectra: recurrence,
                phase: phase, endBeat: n, middleLevels: beats.middleActivity.values,
                openingEnvelope: openingFeatures.envelope, vocalActivity: vocal, lowPeaks: beats.lowPeaks,
                activity: rhythm.secondAttacks, lowQuarters: beats.lowQuarterPeaks,
                powerQuarters: beats.lowQuarterPeaks, converterParameter: 0)
        }
        return Decision(timing: timing, featureBeatCount: n, tentativeMood: tentative, mood: mood, route: route,
            opening: opening, boundaries: boundaries, output: output)
    }

    static func analyze(samples: NativePhraseFeatures.Samples, beats: [Beat], tempo: TempoEstimate?,
                        duration: Double, configuration: AnalysisConfiguration,
                        timingReference: PhraseTimingReference?,
                        nativeGrid: NativeBeatGrid.Result?) throws -> PhraseAnalysis {
        let source: PhraseTimingSource = timingReference == nil ? .independentBeats : .externalReference
        var diagnostics = PhraseDiagnostics()
        diagnostics.measuredTimingCount = timingReference?.beatTimes.count ?? beats.count
        diagnostics.recordedCounters = ["measuredTimingCount"]
        func unknown(_ reason: String) throws -> PhraseAnalysis {
            let phrases = duration > 0 ? [try TrackPhrase(start: 0, end: duration,
                label: .init(kind: .unknown), confidence: 0, source: .inferredAudio)] : []
            return try PhraseAnalysis(mood: configuration.phraseMood, phrases: phrases, timingSource: source,
                warnings: [reason], diagnostics: diagnostics)
        }
        guard duration > 0, let tempo, !beats.isEmpty, configuration.beatsPerBar == 4 else {
            return try unknown("Phrase classification requires a four-beat timing grid.")
        }
        guard samples.low.contains(where: { $0 != 0 }) else { return try unknown("No active audio for phrase classification.") }
        let bpm = ((timingReference?.tempoBPM ?? tempo.bpm) * 100).rounded() * 0.01
        let period = 60 / bpm
        let inputTiming: NativePhraseFeatures.Timing
        if let reference = timingReference {
            inputTiming = try NativePhraseFeatures.referenceTiming(bpm: bpm, firstBeatTime: reference.beatTimes[0],
                firstBeatPosition: reference.beatPositions?.first, firstBeatTempoBPM: reference.firstBeatTempoBPM)
        } else if let grid = nativeGrid, grid.firstBeat.offsetMilliseconds >= 0,
                  (1...4).contains(grid.firstBeat.phase) {
            inputTiming = .init(bpm: Double(grid.globalBPM100) * 0.01,
                offsetMilliseconds: Int(grid.firstBeat.offsetMilliseconds), phase: grid.firstBeat.phase)
        } else { return try unknown("Insufficient complete bars for phrase classification.") }
        let first = Double(inputTiming.offsetMilliseconds) * 0.001
        guard duration - first >= period * 8 else { return try unknown("Insufficient complete bars for phrase classification.") }
        let decision = try classify(samples: samples, timing: inputTiming,
            fixedGrid: true, forcedMood: configuration.phraseMood)
        if let reference = timingReference, decision.output.endBeat > reference.beatTimes.count {
            throw PhraseError.invalidBeatGrid
        }
        // HighPrecision clears the job's Urahaku flag before phrase analysis.
        // The model/Focus grid therefore bypasses the legacy offset correction.
        let clock: (Int) -> Double? = { ordinal in
            guard ordinal > 0 else { return nil }
            if let reference = timingReference {
                return ordinal <= reference.beatTimes.count ? reference.beatTimes[ordinal - 1] : nil
            }
            if let grid = nativeGrid, !grid.beats.isEmpty {
                let time = Double(grid.beats[min(ordinal, grid.beats.count) - 1].microseconds) * 0.000001
                return min(duration, max(0, time - samples.leadingSeconds))
            }
            return nil
        }
        var phrases = try decision.output.phrases(confidence: 0.75,
            omitCollapsedIntervals: timingReference == nil, sampleRate: samples.sourceRate, timeAtBeat: clock).map { phrase in
            try TrackPhrase(start: phrase.start, end: phrase.end,
                label: configuration.inferPhraseStyles ? phrase.label : .init(kind: phrase.label.kind),
                confidence: phrase.confidence, source: .inferredAudio,
                fillIn: configuration.detectFillIns ? phrase.fillIn : nil, referenceCode: phrase.referenceCode)
        }
        diagnostics.selectedBoundaryCount = decision.boundaries.count
        diagnostics.emittedFillCount = phrases.filter { $0.fillIn != nil }.count
        diagnostics.bandLevelObservationCount = decision.featureBeatCount
        diagnostics.recordedCounters = ["measuredTimingCount", "selectedBoundaryCount", "emittedFillCount", "bandLevelObservationCount"]
        if let start = phrases.first?.start, start > 0 {
            phrases.insert(try TrackPhrase(start: 0, end: start, label: .init(kind: .unknown), confidence: 0, source: .inferredAudio), at: 0)
        }
        if let end = phrases.last?.end, end < duration {
            phrases.append(try TrackPhrase(start: end, end: duration, label: .init(kind: .unknown), confidence: 0, source: .inferredAudio))
        }
        var warnings = ["Constant-tempo phrase classification; confidence is not calibrated."]
        if let reference = timingReference, reference.tempoBPM == nil {
            warnings.append("Global phrase tempo was estimated from reference timestamps. Supply tempoBPM to reproduce the original classifier input.")
        }
        return try PhraseAnalysis(mood: decision.mood, phrases: phrases, timingSource: source,
            warnings: warnings, diagnostics: diagnostics)
    }

}
