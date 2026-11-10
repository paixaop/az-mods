#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Cascaded detector filters retain Double delay state and round to Float only
/// after the last section. The order matters for adaptive peak selection.
struct NativeBeatFilters: Sendable {
    struct Section: Sendable {
        let b0: Double
        let b1: Double
        let b2: Double
        let a1: Double
        let a2: Double
        var x1 = 0.0
        var x2 = 0.0
        var y1 = 0.0
        var y2 = 0.0

        mutating func process(_ input: Double) -> Double {
            let feedback = (y1 * a1).addingProduct(y2, a2)
            let numerator = (input * b0).addingProduct(x1, b1).addingProduct(x2, b2)
            let output = numerator - feedback
            x2 = x1
            x1 = input
            y2 = y1
            y1 = output
            return output
        }
    }

    private var sections: [Section]

    static func through() -> Self {
        .init(sections: Array(repeating: .init(b0: 1, b1: 0, b2: 0, a1: 0, a2: 0), count: 2))
    }

    static func low(cutoff: Float, sampleRate: Float, damping: Float) -> Self {
        let section = lowSection(cutoff: cutoff, sampleRate: sampleRate, damping: damping)
        return .init(sections: [section, section])
    }

    static func high(cutoff: Float, sampleRate: Float, damping: Float) -> Self {
        let section = highSection(cutoff: cutoff, sampleRate: sampleRate, damping: damping)
        return .init(sections: [section, section])
    }

    static func middle(lower: Float, upper: Float, sampleRate: Float, damping: Float) -> Self {
        let high = highSection(cutoff: lower, sampleRate: sampleRate, damping: damping)
        let low = lowSection(cutoff: upper, sampleRate: sampleRate, damping: damping)
        return .init(sections: [high, low, high, low])
    }

    static func band(center: Float, sampleRate: Float, width: Float) -> Self {
        let angle = Double(center) * 6.28318530717958 / Double(sampleRate)
        let sine = sin(angle)
        let alpha = sinh(Double(width) * 0.34657359027997264 * angle / sine) * sine
        let scale = 1 / (alpha + 1)
        let section = Section(b0: alpha * scale, b1: 0, b2: -alpha * scale,
                              a1: cos(angle) * -2 * scale, a2: scale.addingProduct(-alpha, scale))
        return .init(sections: [section, section])
    }

    private static func highSection(cutoff: Float, sampleRate: Float, damping: Float) -> Section {
        let angle = Double(cutoff) * 6.28318530717958 / Double(sampleRate)
        let cosine = cos(angle)
        let alpha = sin(angle) / (Double(damping) + Double(damping))
        let scale = 1 / (alpha + 1)
        let b0 = 0.5.addingProduct(cosine, 0.5) * scale
        return Section(b0: b0, b1: -(cosine + 1) * scale, b2: b0,
                       a1: cosine * -2 * scale, a2: scale.addingProduct(-alpha, scale))
    }

    private static func lowSection(cutoff: Float, sampleRate: Float, damping: Float) -> Section {
        let angle = Double(cutoff) * 6.28318530717958 / Double(sampleRate)
        let cosine = cos(angle)
        let alpha = sin(angle) / (Double(damping) + Double(damping))
        let scale = 1 / (alpha + 1)
        let b0 = 0.5.addingProduct(-cosine, 0.5) * scale
        return Section(b0: b0, b1: scale.addingProduct(-cosine, scale), b2: b0,
                       a1: cosine * -2 * scale, a2: scale.addingProduct(-alpha, scale))
    }

    mutating func process(_ input: [Float]) -> [Float] {
        var output = [Float]()
        output.reserveCapacity(input.count)
        for value in input {
            var sample = Double(value)
            for index in sections.indices { sample = sections[index].process(sample) }
            output.append(Float(sample))
        }
        return output
    }
}
