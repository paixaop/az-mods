#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativeBeatInfoAnalyzer {
    struct Result: Sendable {
        var beats: [NativeBeatInfo]
        var globalBPM: Float
        var detectedDynamic: Bool
        var extraBool: Bool
    }

    static func analyze(candidates: [NativeBeatInfo], low: [Int16], sampleRate: Int,
                        sourceFrameCount: Int) throws -> Result {
        var constant = constantPreparation(candidates, sampleRate: sampleRate,
                                           sourceFrameCount: sourceFrameCount)
        guard !constant.segments.isEmpty else {
            return .init(beats: [], globalBPM: 0, detectedDynamic: false, extraBool: false)
        }
        try NativeBeatGridRepair.alignSegments(&constant.segments, candidates: constant.candidates,
                                                sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        let trackedConstant = try track(constant.candidates, segments: constant.segments,
                                        sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        let constantAnchor = trackedConstant.firstReliable < 0 ? 0
            : trackedConstant.beats[trackedConstant.firstReliable].sampleIndex
        var constantGrid = NativeBeatGridRepair.correctSpan(trackedConstant.beats, sampleRate: sampleRate,
            sourceFrameCount: sourceFrameCount, first: trackedConstant.firstReliable, last: trackedConstant.lastReliable)
        guard let first = constantGrid.first else {
            return .init(beats: [], globalBPM: 0, detectedDynamic: false, extraBool: false)
        }
        let constantBPM: Float = 60000 / first.periodMilliseconds
        numberAndTrim(&constantGrid, anchor: constantAnchor, sourceFrameCount: sourceFrameCount)
        let peakDynamic = NativeBeatGridRepair.judgeDynamicPeak(candidates: candidates, low: low,
            sampleRate: sampleRate, sourceFrameCount: sourceFrameCount, globalBPM: constantBPM)

        var variable = try variablePreparation(candidates, sampleRate: sampleRate,
                                               sourceFrameCount: sourceFrameCount)
        var variableBPM: Float = 0
        var longest = 0
        for index in variable.segments.indices {
            let end = index + 1 < variable.segments.count ? variable.segments[index + 1].sampleIndex : sourceFrameCount
            let length = end - (index == 0 ? 0 : variable.segments[index].sampleIndex)
            if length > longest {
                longest = length
                variableBPM = 60000 / variable.segments[index].periodMilliseconds
            }
        }
        // Both alignment calls consume the first prepared candidate stream. The
        // varying-tempo tracker subsequently uses its separately pruned stream.
        try NativeBeatGridRepair.alignSegments(&variable.segments, candidates: constant.candidates,
                                                sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        let trackedVariable = try track(variable.candidates, segments: variable.segments,
                                        sampleRate: sampleRate, sourceFrameCount: sourceFrameCount)
        var variableGrid = trackedVariable.beats
        NativeBeatGridRepair.correctGaps(&variableGrid)
        numberAndTrim(&variableGrid, anchor: trackedVariable.firstAnchor, sourceFrameCount: sourceFrameCount)
        let selection = NativeBeatDynamics.select(variableGrid: variableGrid, sampleRate: sampleRate,
            constantBPM: constantBPM, variableBPM: variableBPM, peakDynamic: peakDynamic)
        return .init(beats: selection.selectVariable ? variableGrid : constantGrid,
            globalBPM: selection.selectVariable ? selection.refinedVariableBPM : selection.refinedConstantBPM,
            detectedDynamic: selection.selectVariable, extraBool: selection.extraBool)
    }

    private static func numberAndTrim(_ beats: inout [NativeBeatInfo], anchor: Int,
                                      sourceFrameCount: Int) {
        let preceding = max(0, beats.lastIndex(where: { $0.sampleIndex <= anchor }) ?? 0)
        let offset = 4 - preceding % 4
        for index in beats.indices { beats[index].beatInBar = (offset + index) % 4 + 1 }
        beats.removeAll { $0.sampleIndex < 0 || $0.sampleIndex > sourceFrameCount }
    }
}
