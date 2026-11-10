#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

extension NativePhraseOpening {
    struct SpectralFeatures {
        let presence: [UInt8]
        let rises: [UInt8]
        let envelope: [Double]
    }

    static func firstSpectralFrame(sampleRate: Double, bpm: Double, firstBeat: Int, offsetMilliseconds: Int) -> Int {
        Int(ceil((sampleRate * 0.00390625)
            * (Double(firstBeat - 1) * 60 / bpm).addingProduct(Double(offsetMilliseconds), 0.001)))
    }

    static func makeSpectra(samples: NativePhraseFeatures.Samples, timing: NativePhraseFeatures.Timing,
                            coordinates: NativePhraseFeatures.Coordinates) throws -> [[Double]] {
        let blocks = try NativePhraseRhythmFeatures.blocks(samples: samples, timing: timing, coordinates: coordinates)
        // Each native call carries everything after its final hop into the next
        // call, so this is the same continuous frame grid through the last block.
        return try makeSpectra(samples: Array(samples.middle.prefix(blocks.last?.middle.upperBound ?? 0)),
            sampleRate: samples.sourceRate / 8)
    }

    /// Constant-Q magnitudes on the original 256-sample hop. Rows are pitch
    /// bins; the first 23 bins remain zero, as in the analysis producer.
    static func makeSpectra(samples: [Double], sampleRate: Double) throws -> [[Double]] {
        try Task.checkCancellation()
        let bank = spectralKernels(sampleRate: sampleRate)
        let longest = bank[0].real.count
        let frames = max(0, (samples.count - longest) / 256)
        var output = Array(repeating: [Double](repeating: 0, count: frames), count: 72)
        guard frames > 0 else { return output }
        for bin in 23..<72 {
            let kernel = bank[bin], length = kernel.real.count
            let scale = Double(longest) / Double(length)
            for frame in 0..<frames {
                if frame % 64 == 0 { try Task.checkCancellation() }
                let start = frame * 256 + longest / 2 - length / 2
                var real = SIMD8<Double>.zero, imaginary = SIMD8<Double>.zero
                let full = length / 8 * 8
                for i in stride(from: 0, to: full, by: 8) {
                    for lane in 0..<8 {
                        let sample = samples[start + i + lane]
                        real[lane] = real[lane].addingProduct(kernel.real[i + lane], sample)
                        imaginary[lane] = imaginary[lane].addingProduct(kernel.imaginary[i + lane], sample)
                    }
                }
                var r = ((real[6] + (real[4] + (real[2] + real[0])))
                    + (real[7] + (real[5] + (real[3] + real[1]))))
                var im = ((imaginary[6] + (imaginary[4] + (imaginary[2] + imaginary[0])))
                    + (imaginary[7] + (imaginary[5] + (imaginary[3] + imaginary[1]))))
                for i in full..<length {
                    r = r.addingProduct(kernel.real[i], samples[start + i])
                    im = im.addingProduct(kernel.imaginary[i], samples[start + i])
                }
                output[bin][frame] = scale * sqrt((r * r).addingProduct(im, im))
            }
        }
        return output
    }

    struct SpectralKernel {
        let real: [Double]
        let imaginary: [Double]
    }

    static func spectralKernels(sampleRate: Double) -> [SpectralKernel] {
        precondition(sampleRate > 0)
        let frequencies = (0...72).map { exp2(Double($0) * 0.08333333333333333) * 27.5 }
        let lengths = (0..<72).map { Int((sampleRate + sampleRate) / (frequencies[$0 + 1] - frequencies[$0])) }
        precondition(lengths.last! > 0)
        var output = [SpectralKernel]()
        for bin in 0..<72 {
            let length = lengths[bin]
            let frequency = frequencies[bin % 12]
            let angular = frequency * 6.283185307179586
            let reciprocalRate = 1 / sampleRate
            var real = [Double](repeating: 0, count: length), imaginary = real
            for i in 0..<length {
                let angle = (Double(i + 1) * reciprocalRate) * angular
                var r = cos(angle), im = sin(angle)
                for _ in 0..<(bin / 12) {
                    let next = (r * r).addingProduct(-im, im)
                    im = r * (im + im); r = next
                }
                real[i] = r; imaginary[i] = im
            }
            let reciprocalLength = 1 / Double(length + 1)
            for i in 0..<(length / 2) {
                let window = 0.5 - cos(Double(i + 1) * 6.283185307179586 * reciprocalLength) * 0.5
                real[i] *= window; imaginary[i] *= window
                real[length - i - 1] *= window; imaginary[length - i - 1] *= window
            }
            if length % 2 == 1 {
                let center = length / 2
                let window = 0.5 - cos(Double(center + 1) * 6.283185307179586 / Double(length + 1)) * 0.5
                real[center] *= window; imaginary[center] *= window
            }
            output.append(SpectralKernel(real: real, imaginary: imaginary))
        }
        return output
    }

    /// Converts the spectral peak counts into bar activity. Byte outputs retain
    /// native beat-sized allocation; only the bar-count prefix is populated.
    static func spectralFeatures(spectra: [[Double]], frameCount: Int, firstFrame: Int, beatCount: Int) -> SpectralFeatures {
        precondition(spectra.count == 72 && frameCount >= 0 && firstFrame >= 0 && beatCount >= 0)
        precondition(spectra.allSatisfy { $0.count >= frameCount })
        let maximum = spectra.flatMap { $0 }.max() ?? 0
        guard maximum > 0 else { return SpectralFeatures(presence: [0], rises: [0], envelope: [0]) }
        let scale = 1 / maximum
        var counts = [Double](repeating: 0, count: frameCount)
        for frame in 0..<frameCount {
            var peak = spectra[0][frame] * scale, rising = false, armed = false
            for bin in 1..<72 {
                let current = spectra[bin][frame] * scale, previous = spectra[bin - 1][frame] * scale
                if current <= previous {
                    if rising {
                        if peak < previous { peak = previous; armed = true }
                    } else if armed {
                        if current < peak * 0.9 {
                            if current > 0.001 && peak > 0.1 { counts[frame] += 1 }
                            armed = false; peak = 0
                        }
                    } else { armed = false }
                }
                rising = previous < current
            }
        }
        let bars = beatCount / 4
        var levels = [Double](repeating: 0, count: bars)
        var position = firstFrame, previousEnd = 0
        for bar in 0..<bars {
            let end = Int((1 / Double(beatCount)) * Double(frameCount) * 4 * Double(bar + 1) + 0.5)
            if previousEnd < end {
                let count = end - previousEnd
                precondition(position + count <= counts.count)
                levels[bar] = NativePhraseFeatures.sum(counts, start: position, count: count)
                position += count
            }
            previousEnd = position
        }
        let divisor = max(1, Int(levels.max() ?? 0))
        levels = levels.map { $0 * (1 / Double(divisor)) }
        var presence = [UInt8](repeating: 0, count: bars * 4), rises = presence
        for i in 0..<bars {
            if levels[i] > 0.36 { presence[i] = 1 }
            if i > 0 {
                if presence[i - 1] < presence[i] {
                    if levels[i - 1] * 1.2 < levels[i] { rises[i] = 1 }
                } else if levels[i] > 0.2 && levels[i] / levels[i - 1] > 1.8 { rises[i] = 1 }
            }
        }
        return SpectralFeatures(presence: presence, rises: rises,
            envelope: levels.flatMap { Array(repeating: $0, count: 4) })
    }
}
