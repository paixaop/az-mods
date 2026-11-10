#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Quarter-rate envelopes used by the tempo and beat-position refinements.
/// Input batches use the analyzer's 75 Hz cadence and interleaved signed PCM.
final class NativeBeatAdjust {
    let sampleRate: Int
    let sourceFrameCount: Int
    let capacity: Int
    private(set) var high: [Int16] = []
    private(set) var low: [Int16] = []
    private(set) var full: [Int16] = []
    private var highPass: Filter
    private var highEnvelope: Filter
    private var lowPass: Filter
    private var lowEnvelope: Filter
    private var fullEnvelope: Filter

    init(sampleRate: Int, sourceFrameCount: Int) {
        precondition([44_100, 48_000].contains(sampleRate) && sourceFrameCount >= 0)
        self.sampleRate = sampleRate / 4
        self.sourceFrameCount = sourceFrameCount
        capacity = min(sourceFrameCount / 4, self.sampleRate * 900)
        high.reserveCapacity(capacity + 1)
        low.reserveCapacity(capacity + 1)
        full.reserveCapacity(capacity + 1)
        let rate = Double(self.sampleRate)
        highPass = Filter(cutoff: 2_000, sampleRate: rate, highPass: true)
        highEnvelope = Filter(cutoff: 1_000, sampleRate: rate, highPass: false)
        lowPass = Filter(cutoff: 100, sampleRate: rate, highPass: false)
        lowEnvelope = Filter(cutoff: 25, sampleRate: rate, highPass: false)
        fullEnvelope = Filter(cutoff: 1_000, sampleRate: rate, highPass: false)
    }

    func append(interleaved samples: [Int16], frameCount: Int) {
        precondition(frameCount >= 0 && frameCount <= samples.count / 2)
        let remaining = sourceFrameCount - low.count * 4
        var count = frameCount
        if remaining < count { count = remaining / 4 * 4 }
        count = min(count, (sampleRate * 900 - low.count) * 4)
        guard count > 0 else { return }
        for frame in stride(from: 0, to: count, by: 4) {
            let mono = (Double(samples[frame * 2 + 1]) + Double(samples[frame * 2])) * 0.5
            let upper = highEnvelope.process(abs(highPass.process(mono)))
            let lower = lowEnvelope.process(abs(lowPass.process(mono)))
            let all = fullEnvelope.process(abs(mono))
            high.append(Self.quantize(upper))
            low.append(Self.quantize(lower))
            full.append(Self.quantize(all))
        }
    }

    private static func quantize(_ value: Double) -> Int16 {
        Int16(max(0, min(32_767, value)))
    }

    func lowIntegral(bpm: Float, sourceSampleIndex: Int) -> Double {
        integral(low, bpm: bpm, sourceSampleIndex: sourceSampleIndex, durationFactor: 30)
    }

    func fullIntegral(bpm: Float, sourceSampleIndex: Int) -> Double {
        integral(full, bpm: bpm, sourceSampleIndex: sourceSampleIndex, durationFactor: 60)
    }

    private func integral(_ values: [Int16], bpm: Float, sourceSampleIndex: Int,
                          durationFactor: Double) -> Double {
        guard bpm.isFinite, abs(bpm) > Float.ulpOfOne, sourceSampleIndex > -4 else { return -1 }
        let count = Int((Double(sampleRate) * durationFactor) / Double(bpm) + 0.5)
        let start = Int(Double(sourceSampleIndex) * 0.25 + 0.5)
        guard start + count - 1 <= capacity else { return -1 }
        guard count > 0 else { return 0 }
        guard start >= 0, start + count <= values.count else { return -1 }
        // Integer envelope values sum exactly in Double at the supported lengths.
        return values[start..<(start + count)].reduce(0) { $0 + Double($1) }
    }

    /// The coefficient routine returns sine and cosine together. Both outputs
    /// are required; a decompiler treating it as a scalar call loses cosine.
    struct Filter {
        let b0: Double, b1: Double, b2: Double, a1: Double, a2: Double
        private var x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0

        init(cutoff: Double, sampleRate: Double, highPass: Bool) {
            let angle = (cutoff * 6.28318530717958) / sampleRate
            let sine = sin(angle), cosine = cos(angle)
            let alpha = sine * 0.7071067811865476
            let scale = 1 / (alpha + 1)
            if highPass {
                b0 = 0.5.addingProduct(cosine, 0.5) * scale
                b1 = -(cosine + 1) * scale
            } else {
                b0 = 0.5.addingProduct(-cosine, 0.5) * scale
                b1 = scale.addingProduct(-cosine, scale)
            }
            b2 = b0
            a1 = (cosine * -2) * scale
            a2 = scale.addingProduct(-alpha, scale)
        }

        mutating func process(_ input: Double) -> Double {
            let feedback = (y1 * a1).addingProduct(y2, a2)
            let feedforward = (input * b0).addingProduct(x1, b1).addingProduct(x2, b2)
            let result = feedforward - feedback
            x2 = x1; x1 = input
            y2 = y1; y1 = result
            return result
        }
    }
}
