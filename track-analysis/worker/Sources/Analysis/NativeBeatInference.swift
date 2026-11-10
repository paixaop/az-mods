#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// High-precision model scheduling and its event corrections. Positions remain
/// relative to `Plan.sourceRange`; the focus stage adds that range's origin.
enum NativeBeatInference {
    struct Plan: Equatable, Sendable {
        let sourceRange: Range<Int>
        let sampleRate: Int
        let dynamic: Bool
        let blocks: [Range<Int>]

        // CommonSettings stores model7 first and model8 second. The original
        // PCM wrapper selects its first path for dynamic timing and second for
        // normal timing; the caller's storage order is not the mode order.
        var modelVariant: NativeBeatModel.Variant { dynamic ? .seven : .eight }
    }
    struct Result: Equatable, Sendable {
        let windowOffset: Int
        let sampleCount: Int
        let export: NativeBeatExport.Result
    }
    enum Failure: Error { case invalidClock, invalidSampleCount }

    static func plan(sourceFrameCount: Int, sampleRate: Int, requestedOffset: Int, dynamic: Bool) throws -> Plan {
        guard sourceFrameCount > 0, sampleRate > 0, requestedOffset >= 0 else { throw Failure.invalidClock }
        let usable = sourceFrameCount - 1
        let minute = sampleRate * 60
        let count: Int, offset: Int
        if dynamic {
            count = usable; offset = 0
        } else {
            if UInt64(usable) &- UInt64(requestedOffset) <= UInt64(minute) {
                count = min(usable, minute)
                offset = max(0, usable - minute)
            } else {
                count = minute
                offset = requestedOffset
            }
        }
        var blocks: [Range<Int>] = []
        if dynamic {
            let minutes = count / minute
            if minutes == 0 {
                if count > 0 { blocks.append(0..<count) }
            } else {
                if minutes > 1 {
                    for index in 0..<(minutes - 1) { blocks.append((index * minute)..<((index + 1) * minute)) }
                }
                let tailStart = (minutes - 1) * minute
                let remainder = count - minutes * minute
                let firstTailCount = remainder > 0 && remainder < sampleRate * 30 ? (count - tailStart) / 2 : minute
                let split = tailStart + firstTailCount
                blocks.append(tailStart..<split)
                if split < count { blocks.append(split..<count) }
            }
        } else if count > 0 { blocks.append(0..<count) }
        return Plan(sourceRange: offset..<(offset + count), sampleRate: sampleRate, dynamic: dynamic, blocks: blocks)
    }

    /// The reader supplies arithmetic stereo-average Float32 PCM in the encoded
    /// source clock. A second file pass can supply each range without retaining
    /// the whole recording while the preliminary timing analysis runs.
    static func analyze(plan: Plan, backend: NativeBeatModel.Backend = .automatic,
                        readMono: (Range<Int>) throws -> [Float]) throws -> Result {
        var events: [NativeBeatExport.Event] = []
        let ordinaryChunks = max(0, plan.sourceRange.count / (plan.sampleRate * 60) - 1)
        for (index, block) in plan.blocks.enumerated() {
            try Task.checkCancellation()
            let absolute = (plan.sourceRange.lowerBound + block.lowerBound)..<(plan.sourceRange.lowerBound + block.upperBound)
            let mono = try readMono(absolute)
            guard mono.count == block.count else { throw Failure.invalidSampleCount }
            let probabilities = try NativeBeatModel.activations(mono: mono, variant: plan.modelVariant, backend: backend)
            let tracked = try NativeBeatHMM.track(activations: probabilities)
            var current = tracked.map { NativeBeatExport.Event(seconds: $0.seconds, beatInBar: $0.beatInBar) }
            if plan.dynamic, block.lowerBound > 0 {
                let offset: Float = index < ordinaryChunks ?
                    (Float(plan.sampleRate) * Float(60.0 / 44_100.0)) * Float(index) :
                    Float(block.lowerBound) * Float(1.0 / 44_100.0)
                for row in current.indices { current[row].seconds += offset }
                if index >= ordinaryChunks, let previous = events.last, !current.isEmpty {
                    var closest = 0
                    while closest + 1 < current.count,
                          abs(previous.seconds - current[closest + 1].seconds) < abs(previous.seconds - current[closest].seconds) {
                        closest += 1
                    }
                    current.removeFirst(closest + 1)
                }
            }
            events.append(contentsOf: current)
        }
        events = corrected(events: events, sampleRate: plan.sampleRate,
                           sampleCount: plan.sourceRange.count, dynamic: plan.dynamic)
        let exported = try NativeBeatExport.make(events: events, sampleCount: plan.sourceRange.count,
                                                 sampleRate: plan.sampleRate)
        return Result(windowOffset: plan.sourceRange.lowerBound, sampleCount: plan.sourceRange.count, export: exported)
    }

    static func corrected(events input: [NativeBeatExport.Event], sampleRate: Int,
                          sampleCount: Int, dynamic: Bool) -> [NativeBeatExport.Event] {
        var events = input
        if sampleRate != 44_100 {
            // The original SIMD prefix leaves one through four scalar rows.
            // Preserve both Float32 evaluation orders at this boundary.
            let scalarCount = events.count % 4 == 0 ? 4 : events.count % 4
            let vectorEnd = events.count > 4 ? events.count - scalarCount : 0
            for index in events.indices {
                events[index].seconds = index < vectorEnd ? events[index].seconds * (44_100 / Float(sampleRate)) :
                    (events[index].seconds * 44_100) * (1 / Float(sampleRate))
            }
        }
        guard dynamic else { return events }
        let minute = sampleRate * 60, minutes = sampleCount / minute
        let lastStart = max(0, minutes - 1) * minute
        let lastFullCount = minutes > 0 ? minute : sampleCount
        let remainder = sampleCount - lastStart - lastFullCount
        let finalBoundary = minutes > 0 && remainder > 0 && remainder < sampleRate * 30 ?
            lastStart + (sampleCount - lastStart) / 2 : lastStart + lastFullCount
        if minutes > 1 {
            for index in 1..<minutes { correctSeam(&events, boundary: Float(index) * 60) }
        }
        if finalBoundary > lastStart { correctSeam(&events, boundary: Float(finalBoundary) / Float(sampleRate)) }
        let originalCount = events.count
        if originalCount > 2 {
            for index in 1..<(originalCount - 1) {
                let before = events[index].seconds - events[index - 1].seconds
                let gap = events[index + 1].seconds - events[index].seconds
                let ratio = before > Float.ulpOfOne ? gap / before : 0
                let divisions = Int(ratio + 0.5)
                var insert = ratio > 1.8 && divisions > 1
                if index < originalCount - 2 {
                    let after = events[index + 2].seconds - events[index + 1].seconds
                    let otherRatio = after > Float.ulpOfOne ? gap / after : 0
                    insert = insert && Int(otherRatio + 0.5) > 0 && otherRatio > 1.8
                }
                if insert { subdivide(&events, after: index, divisions: divisions) }
            }
        }
        return events
    }

    private static func correctSeam(_ events: inout [NativeBeatExport.Event], boundary: Float) {
        guard let next = events.firstIndex(where: { $0.seconds >= boundary }), next >= 2, next + 1 < events.count else { return }
        let previous = next - 1
        let before = events[previous].seconds - events[previous - 1].seconds
        let gap = events[next].seconds - events[previous].seconds
        let after = events[next + 1].seconds - events[next].seconds
        let firstRatio = before > Float.ulpOfOne ? gap / before : 0
        let secondRatio = gap > Float.ulpOfOne ? after / gap : 0
        let firstDivisions = Int(firstRatio + 0.5), secondDivisions = Int(secondRatio + 0.5)
        if firstDivisions > 1, firstRatio >= 1.35, firstRatio <= 3.5, secondRatio >= 0.25, secondRatio <= 0.75 {
            subdivide(&events, after: previous, divisions: firstDivisions)
        } else if secondDivisions > 1, firstRatio >= 0.9, firstRatio <= 1.1, secondRatio >= 1.35, secondRatio <= 3.5 {
            subdivide(&events, after: next, divisions: secondDivisions)
        } else if firstRatio < 0.3 {
            events.remove(at: next)
        } else if secondRatio < 0.3 {
            events.remove(at: next + 1)
        }
    }

    private static func subdivide(_ events: inout [NativeBeatExport.Event], after index: Int, divisions: Int) {
        let start = events[index].seconds, gap = events[index + 1].seconds - start
        let additions = (1..<divisions).map {
            NativeBeatExport.Event(seconds: start.addingProduct(gap * Float($0), 1 / Float(divisions)), beatInBar: 1)
        }
        events.insert(contentsOf: additions, at: index + 1)
    }
}
