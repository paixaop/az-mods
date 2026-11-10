#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// The publicly licensed madmom 2016 models used by the native beat detector.
/// The resources are rebuilt from upstream weights; no reference executable or
/// reference-model file is included. See Resources/NativeBeatModels/LICENSE.txt.
enum NativeBeatModel {
    enum Variant: Int, Sendable { case seven = 7, eight = 8 }
    enum Backend: Sendable { case automatic, metal, cpu }
    struct Features: Sendable {
        let frameCount: Int
        let values: [Float]
    }
    enum Failure: Error { case resource(String), shape, metalUnavailable }
    struct Tensor: Decodable {
        let shape: [Int]
        let data: Data
        var values: [Float] { data.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) } }
    }

    static let hop = 441
    static let featureCount = 314
    private static let sizes = [1_024, 2_048, 4_096]
    private static let bands = [21, 45, 91]

    static func features(mono: [Float], backend: Backend = .automatic) throws -> Features {
        guard !mono.isEmpty else { return Features(frameCount: 0, values: []) }
        let frames = mono.count / hop + 1
        let filterbanks = try resources("filterbanks")
        if backend == .metal { throw Failure.metalUnavailable }
        var result = [Float](repeating: 0, count: frames * featureCount)
        var column = 0
        for (index, size) in sizes.enumerated() {
            try Task.checkCancellation()
            guard let filters = filterbanks[String(size)], filters.shape == [size / 2, bands[index]] else {
                throw Failure.shape
            }
            let levels = try cpuSpectra(mono: mono, frames: frames, size: size, filters: filters)
            let width = bands[index]
            let difference = index == 2 ? 2 : 1
            for frame in 0..<frames {
                for band in 0..<width {
                    let value = levels[frame * width + band]
                    result[frame * featureCount + column + band] = value
                    result[frame * featureCount + column + width + band] = frame < difference ? 0 :
                        max(0, value - levels[(frame - difference) * width + band])
                }
            }
            column += width * 2
        }
        return Features(frameCount: frames, values: result)
    }

    /// Interleaved beat/downbeat probabilities, sampled at 100 frames per second
    /// in the model's 44.1-kHz clock. The caller applies source-rate conversion.
    static func activations(features: Features, variant: Variant,
                            backend: Backend = .automatic) throws -> [Float] {
        guard features.values.count == features.frameCount * featureCount else { throw Failure.shape }
        guard features.frameCount > 0 else { return [] }
        let weights = try resources("model\(variant.rawValue)")
        if backend == .metal { throw Failure.metalUnavailable }
        return try cpuNetwork(features: features, weights: weights)
    }

    static func activations(mono: [Float], variant: Variant,
                            backend: Backend = .automatic) throws -> [Float] {
        try activations(features: features(mono: mono, backend: backend), variant: variant, backend: backend)
    }

    private static func resources(_ name: String) throws -> [String: Tensor] {
        let arguments = CommandLine.arguments
        let directory: String
        if let configured = getenv("TRACK_ANALYSIS_MODELS") { directory = String(cString: configured) }
        else if arguments.count == 6 { directory = arguments[5] }
        else { throw Failure.resource("Set TRACK_ANALYSIS_MODELS for library tests") }
        let url = URL(fileURLWithPath: directory).appendingPathComponent(name + ".plist")
        return try PropertyListDecoder().decode([String: Tensor].self, from: Data(contentsOf: url))
    }

    private static func window(_ size: Int) -> [Float] {
        (0..<size).map { 0.5 - 0.5 * cosf((Float.pi * 2 * Float($0)) / Float(size)) }
    }

    private static func frame(_ mono: [Float], index: Int, size: Int, window: [Float], destination: inout [Float]) {
        let origin = index * hop - size / 2
        for sample in 0..<size {
            let source = origin + sample
            destination[sample] = source >= 0 && source < mono.count ? mono[source] * window[sample] : 0
        }
    }

    private static func cpuSpectra(mono: [Float], frames: Int, size: Int, filters: Tensor) throws -> [Float] {
        let width = filters.shape[1], weights = filters.values, hann = window(size)
        var real = [Float](repeating: 0, count: size)
        var imaginary = real
        var magnitudes = [Float](repeating: 0, count: size / 2)
        var projected = [Float](repeating: 0, count: width)
        var result = [Float](repeating: 0, count: frames * width)
        for time in 0..<frames {
            if time % 128 == 0 { try Task.checkCancellation() }
            frame(mono, index: time, size: size, window: hann, destination: &real)
            imaginary.withUnsafeMutableBufferPointer { $0.initialize(repeating: 0) }
            ta_fft_f(&real, &imaginary, size)
            for bin in 0..<size / 2 { magnitudes[bin] = hypotf(real[bin], imaginary[bin]) }
            cblas_sgemv(CblasRowMajor, CblasTrans, Int32(size / 2), Int32(width), 1,
                        weights, Int32(width), magnitudes, 1, 0, &projected, 1)
            for band in 0..<width { result[time * width + band] = logf(1 + projected[band]) / logf(10) }
        }
        return result
    }

    private static func cpuNetwork(features: Features, weights: [String: Tensor]) throws -> [Float] {
        let frames = features.frameCount
        func floats(_ name: String) throws -> [Float] {
            guard let tensor = weights[name] else { throw Failure.resource(name) }
            return tensor.values
        }
        var source = features.values, width = featureCount
        for layer in 0..<3 {
            try Task.checkCancellation()
            let input = try floats("layer\(layer).input"), recurrent = try floats("layer\(layer).recurrent")
            let bias = try floats("layer\(layer).bias"), peephole = try floats("layer\(layer).peephole")
            var destination = [Float](repeating: 0, count: frames * 50)
            for direction in 0..<2 {
                var projected = [Float](repeating: 0, count: frames * 100)
                input.withUnsafeBufferPointer { matrix in
                    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, Int32(frames), 100, Int32(width), 1,
                                source, Int32(width), matrix.baseAddress!.advanced(by: direction * 100 * width),
                                Int32(width), 0, &projected, 100)
                }
                var state = [Float](repeating: 0, count: 25), cell = state
                var recurrentValue = [Float](repeating: 0, count: 100)
                for iteration in 0..<frames {
                    if iteration % 128 == 0 { try Task.checkCancellation() }
                    let time = direction == 0 ? iteration : frames - 1 - iteration
                    recurrent.withUnsafeBufferPointer { matrix in
                        cblas_sgemv(CblasRowMajor, CblasNoTrans, 100, 25, 1,
                            matrix.baseAddress!.advanced(by: direction * 2_500), 25, state, 1, 0, &recurrentValue, 1)
                    }
                    let base = time * 100, p = direction * 100
                    for unit in 0..<25 {
                        let inputGate = sigmoid((projected[base + unit] + recurrentValue[unit] + bias[p + unit]) + cell[unit] * peephole[p + unit])
                        let forgetGate = sigmoid((projected[base + 25 + unit] + recurrentValue[25 + unit] + bias[p + 25 + unit]) + cell[unit] * peephole[p + 25 + unit])
                        let candidate = tanhf(projected[base + 50 + unit] + recurrentValue[50 + unit] + bias[p + 50 + unit])
                        cell[unit] = forgetGate * cell[unit] + inputGate * candidate
                        let outputGate = sigmoid((projected[base + 75 + unit] + recurrentValue[75 + unit] + bias[p + 75 + unit]) + cell[unit] * peephole[p + 75 + unit])
                        state[unit] = outputGate * tanhf(cell[unit])
                        destination[time * 50 + direction * 25 + unit] = state[unit]
                    }
                }
            }
            source = destination; width = 50
        }
        let matrix = try floats("dense.input"), bias = try floats("dense.bias")
        var logits = [Float](repeating: 0, count: frames * 3)
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, Int32(frames), 3, 50, 1, source, 50, matrix, 3, 0, &logits, 3)
        var result = [Float](repeating: 0, count: frames * 2)
        for frame in 0..<frames {
            let a = logits[frame * 3] + bias[0], b = logits[frame * 3 + 1] + bias[1], c = logits[frame * 3 + 2] + bias[2]
            let maximum = max(a, max(b, c))
            let e0 = expf(a - maximum), e1 = expf(b - maximum), e2 = expf(c - maximum)
            let total = e0 + e1 + e2
            result[frame * 2] = e1 / total; result[frame * 2 + 1] = e2 / total
        }
        return result
    }

    private static func sigmoid(_ value: Float) -> Float { 1 / (1 + expf(-value)) }
}
