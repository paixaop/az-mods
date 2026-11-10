#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Causal envelope stages. Inputs have already been decimated from the same mono
/// stream; this type does not silently substitute a resampler or channel policy.
enum NativePhraseEnvelopes {
    struct Streams {
        let low: [Double]
        let middle: [Double]
        let high: [Double]
        let lowLevel: [Double]
        let middleLevel: [Double]
    }

    /// Transposed direct-form II with the operation order used by the analysis
    /// filters. A section keeps its state across calls.
    struct Section {
        let b0: Double, b1: Double, b2: Double, a1: Double, a2: Double
        var z1 = 0.0, z2 = 0.0
        let firstOrder: Bool

        mutating func process(_ input: [Double]) -> [Double] {
            input.map { x in
                let y = z1.addingProduct(x, b0)
                if firstOrder {
                    z1 = (-y * a1).addingProduct(x, b1)
                } else {
                    z1 = z2.addingProduct(x, b1).addingProduct(-y, a1)
                    z2 = (-y * a2).addingProduct(x, b2)
                }
                return y
            }
        }

        mutating func processCancellable(_ input: [Double]) throws -> [Double] {
            var output = [Double]()
            output.reserveCapacity(input.count)
            for start in stride(from: 0, to: input.count, by: 16_384) {
                try Task.checkCancellation()
                output += process(Array(input[start..<min(input.count, start + 16_384)]))
            }
            return output
        }
    }

    static func secondOrder(rate: Double, angularCutoff: Double, damping: Double, highPass: Bool) -> Section {
        let warped = tan(angularCutoff / (rate + rate)) * (rate + rate)
        let reciprocalRate = 1 / rate
        let denominator2 = reciprocalRate * reciprocalRate * (warped * warped)
        let denominator1 = (reciprocalRate + reciprocalRate) * (warped * damping)
        let scale = 1 / (denominator1 + 4 + denominator2)
        let numerator0 = highPass ? 4.0 : 0
        let numerator2 = highPass ? 0 : reciprocalRate * reciprocalRate * (warped * warped)
        return Section(b0: scale * (numerator0 + numerator2),
            b1: (scale + scale) * (numerator2 - numerator0),
            b2: scale * (numerator0 + numerator2),
            a1: (scale + scale) * (denominator2 - 4),
            a2: scale * ((4 - denominator1) + denominator2), firstOrder: false)
    }

    static func smoother(rate: Double, angularCutoff: Double) -> Section {
        let warped = tan(angularCutoff / (rate + rate)) * (rate + rate)
        let numerator = (1 / rate) * warped
        let scale = 1 / (numerator + 2)
        return Section(b0: scale * numerator, b1: scale * numerator, b2: 0,
            a1: scale * (numerator - (1 + 1)), a2: 0, firstOrder: true)
    }

    static func make(lowInput: [Double], middleInput: [Double], highInput: [Double], sourceRate: Double) throws -> Streams {
        func envelope(_ input: [Double], rate: Double, lower: Double?, upper: Double, smoothing: Double, damping: [Double]) throws -> [Double] {
            var samples = input
            if let lower {
                for factor in damping {
                    var section = secondOrder(rate: rate, angularCutoff: lower, damping: factor, highPass: true)
                    samples = try section.processCancellable(samples)
                }
            }
            for factor in damping {
                var section = secondOrder(rate: rate, angularCutoff: upper, damping: factor, highPass: false)
                samples = try section.processCancellable(samples)
            }
            var section = smoother(rate: rate, angularCutoff: smoothing)
            return try section.processCancellable(samples.map(abs))
        }
        let fourthOrder = [1.8477590650225735, 0.7653668647301797]
        return try Streams(
            low: envelope(lowInput, rate: sourceRate / 32, lower: nil, upper: 628.3185307179587,
                smoothing: 31.41592653589793, damping: [1.4142135623730951]),
            middle: envelope(middleInput, rate: sourceRate / 8, lower: 2513.2741228718346,
                upper: 6283.185307179586, smoothing: 62.83185307179586, damping: fourthOrder),
            high: envelope(highInput, rate: sourceRate / 2, lower: 31415.926535897932,
                upper: 43982.2971502571, smoothing: 62.83185307179586, damping: fourthOrder),
            lowLevel: envelope(lowInput, rate: sourceRate / 32, lower: nil, upper: 942.4777960769379,
                smoothing: 31.41592653589793, damping: [1.4142135623730951]),
            middleLevel: middleLevel(input: middleInput, rate: sourceRate / 8))
    }

    private static func middleLevel(input: [Double], rate: Double) throws -> [Double] {
        var band = secondOrder(rate: rate, angularCutoff: 3141.592653589793,
            damping: 1.4142135623730951, highPass: true)
        var envelope = smoother(rate: rate, angularCutoff: 31.41592653589793)
        return try envelope.processCancellable(band.processCancellable(input).map(abs))
    }
}
