#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativePhraseBoundaries {
    /// All boundary routes, including the native caller's protected-anchor
    /// reconciliation and first/last-boundary rules. Label classification follows this step.
    static func candidates(beats: NativePhraseFeatures.Beats, openingHypotheses: [Int],
                           introEnd: Int, openingFlag: Int, openingEnvelope: [Double],
                           vocalActivity: [Double], hihatChanges: [UInt8], rhythm: [UInt8],
                           durationSeconds: Double, route: Int) -> [Int] {
        guard openingFlag == 1 else { return [beats.initialBoundary] }
        let phase = beats.timing.phase
        let anchors = securePoints(lowPeaks: beats.lowPeaks, phase: phase).beatPositions
        let proposals: [Int]
        if route == 1 {
            proposals = genre1Candidates(beats: beats, openingHypotheses: openingHypotheses,
                introEnd: introEnd, openingEnvelope: openingEnvelope, hihatChanges: hihatChanges,
                rhythm: rhythm, durationSeconds: durationSeconds, anchors: anchors)
        } else {
            proposals = genre23Candidates(beats: beats, openingHypotheses: openingHypotheses,
                introEnd: introEnd, openingEnvelope: openingEnvelope, vocalActivity: vocalActivity, route: route)
        }
        return finishCandidates(proposals, initialBoundary: beats.initialBoundary, anchors: anchors,
            route: route, endBeat: min(beats.middleLevels.count, openingEnvelope.count))
    }

    static func genre1Candidates(beats: NativePhraseFeatures.Beats, openingHypotheses: [Int],
                                 introEnd: Int, openingEnvelope: [Double], hihatChanges: [UInt8],
                                 rhythm: [UInt8], durationSeconds: Double, anchors: [Int]) -> [Int] {
        let phase = beats.timing.phase
        let low = lowPeakCandidates(introEnd: introEnd, changes: beats.lowPeakChanges,
            phase: phase, lowLevelChanges: beats.lowActivity.changes)
        var longGapStart = 0
        for index in low.indices {
            let end = index + 1 < low.count ? low[index + 1] : beats.lowPeakChanges.count
            if end - low[index] >= 113 { longGapStart = low[index]; break }
        }
        let breakInfo = breakEvidence(candidates: low, phase: phase, lowLevels: beats.lowActivity.values,
            middleLevels: beats.middleActivity.values, allLevels: beats.allActivity.bars)
        let changeInfo = changeEvidence(candidates: low, phase: phase, lowLevels: beats.lowActivity.values,
            middleLevels: beats.middleActivity.values, allLevels: beats.allActivity.bars,
            openingEnvelope: openingEnvelope, lowPeaks: beats.lowPeaks)
        let seeds: [Int]
        if longGapStart < 2, breakInfo.start < 2, changeInfo.firstAligned < 2 { seeds = [introEnd] }
        else {
            seeds = irregularSeeds(introEnd: introEnd, changes: beats.lowPeakChanges,
                phase: phase, lowLevelChanges: beats.lowActivity.changes, longGapStart: longGapStart,
                breakInfo: breakInfo, changeInfo: changeInfo)
        }
        let all = allBandCandidates(seeds: reconcileHighSeeds(seeds, anchors: anchors), phase: phase,
            lowChanges: beats.lowPeakChanges, lowLevelChanges: beats.lowActivity.changes,
            middleLevelChanges: beats.middleActivity.changes, hihatChanges: hihatChanges,
            profile: VerseProfile(lowPeaks: beats.lowPeaks, lowLevels: beats.lowActivity.values,
                middleLevels: beats.middleActivity.values, openingEnvelope: openingEnvelope,
                allLevels: beats.allActivity.bars, lowChanges: beats.lowPeakChanges))
        let adjusted = adjustHighCandidates(all, allLevels: beats.allActivity.bars, lowLevels: beats.lowActivity.values)
        let selected = Double(adjusted.count) <= durationSeconds * 0.052000000000000005
            ? adjusted : rejectRepeatedRhythm(adjusted, rhythm: rhythm).candidates
        let middle = NativePhraseFeatures.middlePeakActivity(beats.middlePeaks,
            normalizationCount: beats.coordinates.beatCount, phase: phase)
        let features = candidateFeatures(lowPeaks: beats.lowPeaks,
            lowLevels: beats.lowActivity.values, middlePeaks: middle.normalizedBeats,
            middleCoarsePeaks: middle.bars, phase: phase)
        let secondary = verseHookCandidates(features: features, openingHypotheses: openingHypotheses,
            introEnd: introEnd, phase: phase, route: 1)
        let count = middle.normalizedBeats.filter { $0 != 0 }.count
        let bars = count / 4
        let high = NativePhraseFills.normalizeHigh(beats.highEighthPeaks, phase: phase)?.values ?? []
        return addGenre1Candidates(base: [beats.initialBoundary] + selected, secondary: secondary,
            lowProfile: features.lowLevel, shortBreaks: shortBreakCandidates(lowLevels: beats.lowLevels,
                middlePeaks: middle.normalizedBeats, count: count, barCount: bars),
            snares: NativePhraseFills.snareCandidates(normalizedHigh: high, bars: bars), phase: phase)
    }

    /// Complete genre-two/three proposal route. Inputs retain the producer's
    /// native coordinates; the opening envelope is the IntroEndFind_b profile.
    static func genre23Candidates(beats: NativePhraseFeatures.Beats, openingHypotheses: [Int],
                                  introEnd: Int, openingEnvelope: [Double],
                                  vocalActivity: [Double], route: Int) -> [Int] {
        let phase = beats.timing.phase
        let middle = NativePhraseFeatures.middlePeakActivity(beats.middlePeaks,
            normalizationCount: beats.coordinates.beatCount, phase: phase)
        let features = candidateFeatures(lowPeaks: beats.lowPeaks,
            lowLevels: beats.lowActivity.values, middlePeaks: middle.normalizedBeats,
            middleCoarsePeaks: middle.bars, phase: phase)
        let base = verseHookCandidates(features: features, openingHypotheses: openingHypotheses,
            introEnd: introEnd, phase: phase, route: route)
        let bars = middle.normalizedBeats.filter { $0 != 0 }.count / 4
        let high = NativePhraseFills.normalizeHigh(beats.highEighthPeaks, phase: phase)?.values ?? []
        let snare = NativePhraseFills.snareCandidates(normalizedHigh: high, bars: bars)
        let noise = NativePhraseAdditionalBoundaries.noiseChanges(openingEnvelope)
        let vocal = NativePhraseSpectra.vocalEntries(vocalActivity)
        return addGenre23Candidates(base: base, lowLevels: beats.lowLevels,
            middlePeaks: middle.normalizedBeats, souon: noise, snare: snare, vocal: vocal, phase: phase)
    }
}
