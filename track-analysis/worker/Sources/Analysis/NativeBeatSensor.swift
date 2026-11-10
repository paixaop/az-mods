#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Six-band onset sensing and tempo-hypothesis accumulation. Samples are the
/// detector's equal-channel normalized mono, before any phrase-specific fold.
final class NativeBeatSensor {
    struct Peak: Sendable {
        var sampleIndex: Int
        var amplitude: Float
        var intervalMilliseconds: Float
    }

    private struct Detector {
        let sampleRate: Int
        let holdoff: Int
        let initialDecayWait: Int
        let decayDuration: Int
        var threshold: Float = 0.001
        var decayStep: Float = 0
        var decayStage = 0
        var decayWait = 0
        var lastDecay = 0
        var position = 0
        var collecting = false
        var amplitude: Float = 0
        var peakPosition = 0
        var lastMaximum = 0

        init(sampleRate: Int) {
            self.sampleRate = sampleRate
            holdoff = Int(Float(sampleRate) * 0.29000002)
            initialDecayWait = Int(Float(sampleRate) * 1.7)
            decayDuration = Int(Float(sampleRate) * 1.3)
        }

        mutating func append(_ value: Float) -> (Int, Float)? {
            position += 8
            threshold -= decayStep
            if collecting {
                if value > amplitude {
                    startPeak(value)
                }
                if position - lastMaximum >= holdoff {
                    collecting = false
                    return (peakPosition, amplitude)
                }
            } else {
                threshold = max(threshold, 0.001)
                if position - lastDecay > decayWait {
                    let target: Float
                    if decayStage == 0 {
                        decayStage = 1
                        target = 0.5 * amplitude
                    } else {
                        target = 0.5 * threshold
                    }
                    decayWait = decayDuration
                    let perSample = (threshold - target) / Float(decayDuration)
                    decayStep = perSample * 8
                    lastDecay = position
                }
                if value >= threshold {
                    startPeak(value)
                    collecting = true
                }
            }
            return nil
        }

        private mutating func startPeak(_ value: Float) {
            peakPosition = position
            lastMaximum = position
            amplitude = value
            threshold = 0.75 * value
            decayStage = 0
            decayWait = initialDecayWait
            decayStep = 0
            lastDecay = position
        }
    }

    private struct Difference {
        var ring: [Float]
        var index = 0
        var sum: Float = 0

        mutating func process(_ values: [Float]) -> [Float] {
            let scale = 1 / Float(ring.count)
            return values.map { value in
                sum += value - ring[index]
                let result = max(0, value.addingProduct(-sum, scale))
                ring[index] = value
                index = (index + 1) % ring.count
                return result
            }
        }
    }

    private struct Band {
        var detector: Detector
        var peaks: [Peak] = []
        var lastPeak = 0
        var period: Float = 0
        var error = Float.greatestFiniteMagnitude
        var matchedIntervals = 0
        var accepted = false

        mutating func append(_ value: Float) {
            accepted = false
            guard let (position, amplitude) = detector.append(value) else { return }
            var interval: Float = 0
            if lastPeak != 0 {
                interval = Float(position - lastPeak) * 1000 / Float(detector.sampleRate)
                period = interval
                if let candidate = NativeBeatPeriodCandidates.calculate(interval: interval,
                                                                          previous: peaks.map(\.intervalMilliseconds)) {
                    period = candidate.periodMilliseconds
                    error = candidate.error
                    matchedIntervals = candidate.matchedIntervals
                    accepted = true
                }
            }
            lastPeak = position
            peaks.append(.init(sampleIndex: position, amplitude: amplitude, intervalMilliseconds: interval))
            if peaks.count > 32 { peaks.removeFirst() }
        }
    }

    private let clockRate: Int
    private let frameLimit: Int
    private let blockSize: Int
    private var processed = 0
    private var subdivision = 0
    private var filters: [NativeBeatFilters]
    private var differences: [Difference]
    private var bands: [Band]
    private var sums = [Float](repeating: 0, count: 6)
    private var tempoHistogram = [Int](repeating: 0, count: 256)
    private var lowestTempo = 200
    private var highestTempo = 40
    private var sequence = 0
    private(set) var candidates: [NativeBeatInfo] = []

    init(sampleRate: Int, sourceFrameCount: Int) {
        precondition([44_100, 48_000].contains(sampleRate) && sourceFrameCount > 0)
        clockRate = sampleRate
        frameLimit = sourceFrameCount
        blockSize = sampleRate / 75
        let rate = Float(sampleRate)
        filters = [
            .low(cutoff: 283, sampleRate: rate, damping: 0.70710677),
            .band(center: 150, sampleRate: rate, width: 1.5),
            .middle(lower: 283, upper: 7900, sampleRate: rate, damping: 0.70710677),
            .high(cutoff: 7900, sampleRate: rate, damping: 0.70710677)
        ]
        differences = [
            .init(ring: .init(repeating: 0, count: max(2, Int(rate * 0.001 + 0.5)))),
            .init(ring: .init(repeating: 0, count: max(2, Int(rate * 0.00020000001 + 0.5))))
        ]
        bands = (0..<6).map { _ in .init(detector: .init(sampleRate: sampleRate)) }
    }

    func append(normalizedMono input: [Float]) throws {
        guard processed < frameLimit else { return }
        for first in stride(from: 0, to: input.count, by: blockSize) {
            try Task.checkCancellation()
            let chunk = Array(input[first..<min(first + blockSize, input.count)])
            var levels = filters.indices.map { filters[$0].process(chunk).map(abs) }
            levels.append(differences[0].process(levels[0]))
            levels.append(differences[1].process(levels[3]))
            for frame in chunk.indices {
                processed += 1
                guard processed < frameLimit else { return }
                for band in bands.indices { sums[band] += levels[band][frame] }
                subdivision += 1
                if subdivision == 8 {
                    subdivision = 0
                    for index in bands.indices {
                        bands[index].append(sums[index] / 8)
                        sums[index] = 0
                    }
                    if selectBands() {
                        for index in bands.indices where bands[index].accepted { emit(index) }
                    }
                }
            }
        }
    }

    private static func compatible(_ period: Float, with candidate: Float, tolerance: Float) -> Bool {
        abs(period - candidate) <= tolerance ||
            abs((period + period) - candidate) <= tolerance ||
            abs(period * 0.5 - candidate) <= tolerance
    }

    private func dominantTempo() -> Int? {
        guard highestTempo >= lowestTempo else { return nil }
        var best: Int?
        var maximum = 0
        for tempo in lowestTempo...highestTempo where tempoHistogram[tempo] > maximum {
            maximum = tempoHistogram[tempo]
            best = tempo
        }
        return best
    }

    private func selectBands() -> Bool {
        var selected: Int?
        var error = Float.greatestFiniteMagnitude
        for index in bands.indices where bands[index].accepted {
            let band = bands[index]
            let tempo = Int(60000 / band.period + 0.5)
            tempoHistogram[tempo] += max(1, Int(4 / (band.error + 1)))
            lowestTempo = min(lowestTempo, tempo)
            highestTempo = max(highestTempo, tempo)
            if band.error < error { selected = index; error = band.error }
        }
        guard let selected, var tempo = dominantTempo() else { return false }
        let period = bands[selected].period
        let tolerance = Float(min(Double(period) * 0.04, 20))
        while !Self.compatible(60000 / Float(tempo), with: period, tolerance: tolerance) {
            let selectedTempo = Int(60000 / period + 0.5)
            guard tempoHistogram[selectedTempo] >= 3 else { return false }
            let oldLower = lowestTempo, oldUpper = highestTempo
            lowestTempo = 200
            highestTempo = 40
            for candidate in oldLower...oldUpper {
                if Self.compatible(60000 / Float(candidate), with: period, tolerance: tolerance) {
                    if tempoHistogram[candidate] != 0 {
                        lowestTempo = min(lowestTempo, candidate)
                        highestTempo = max(highestTempo, candidate)
                    }
                } else { tempoHistogram[candidate] = 0 }
            }
            guard let dominant = dominantTempo() else { return false }
            tempo = dominant
        }
        guard tempoHistogram[tempo] >= 3 else { return false }
        let reference = 60000 / Float(tempo)
        for index in bands.indices where bands[index].accepted {
            let bandPeriod = bands[index].period
            if !Self.compatible(reference, with: bandPeriod, tolerance: Float(min(Double(bandPeriod) * 0.04, 20))) {
                bands[index].accepted = false
            }
        }
        return true
    }

    private func emit(_ bandIndex: Int) {
        let band = bands[bandIndex]
        let count = band.matchedIntervals + 2
        sequence += count
        let tempo10 = Int(600000 / band.period + 0.5)
        let tolerance = Float(min(Double(band.period) * 0.04, 20))
        let prefix = [Peak](repeating: .init(sampleIndex: 0, amplitude: 0, intervalMilliseconds: 0),
                            count: max(0, count - band.peaks.count))
        for peak in prefix + band.peaks.suffix(count) {
            let interval = Float(Int((1 / band.period) * peak.intervalMilliseconds * 4 + 0.5)) * 0.25
            var insertion = candidates.count
            while insertion > 0 && candidates[insertion - 1].sampleIndex > peak.sampleIndex { insertion -= 1 }
            var cursor = insertion - 1
            var rejected = false
            while cursor >= 0 {
                let order = compare_timestamps(Double(candidates[cursor].sampleIndex), Double(peak.sampleIndex),
                    UInt64(clockRate), Double(clockRate), 1)
                guard order != AZ_TIME_INVALID, order == AZ_TIME_EQUAL else { break }
                let prior = candidates[cursor]
                let priorTempo = Int(600000 / prior.periodMilliseconds + 0.5)
                if priorTempo == tempo10 || priorTempo * 2 == tempo10 || tempo10 * 2 == priorTempo ||
                    !Self.compatible(prior.periodMilliseconds, with: band.period, tolerance: tolerance) {
                    if (sequence & 255) <= (prior.beatInBar & 255) { rejected = true }
                    else { candidates.remove(at: cursor); insertion -= 1 }
                    break
                }
                cursor -= 1
            }
            if !rejected {
                candidates.insert(.init(sampleIndex: peak.sampleIndex, intervalInBeats: interval,
                    periodMilliseconds: band.period, beatInBar: sequence, sourceBand: bandIndex, flag: 0), at: insertion)
            }
        }
        sequence = (sequence + 256) & ~255
    }
}
