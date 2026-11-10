#if canImport(CryptoKit)
import CryptoKit
#endif
import Foundation
import XCTest
@testable import Analysis

final class NativeBeatModelTests: XCTestCase {
    func testCPUFeaturesAndBothModelsMatchOriginalTensorFlow() throws {
        try compare(backend: .cpu)
    }

    private func compare(backend: NativeBeatModel.Backend) throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatModel", withExtension: "json"))
        let fixture = try JSONDecoder().decode(ModelWitnesses.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for witness in fixture.cases {
            let samples = witness.recipe.samples()
            let digest = try samples.withUnsafeBytes { try fixtureSHA256(Data($0)) }
            XCTAssertEqual(digest, witness.sampleSHA256)
            let features = try NativeBeatModel.features(mono: samples, backend: backend)
            XCTAssertEqual(features.frameCount, witness.frameCount, witness.recipe.name)
            let selected = witness.featureFrameIndices.flatMap { frame in
                Array(features.values[(frame * NativeBeatModel.featureCount)..<((frame + 1) * NativeBeatModel.featureCount)])
            }
            assertClose(selected, witness.expectedFeatures.floats, tolerance: 0.00001,
                        label: "\(witness.recipe.name) features")
            for variant: NativeBeatModel.Variant in [.seven, .eight] {
                let activations = try NativeBeatModel.activations(features: features, variant: variant, backend: backend)
                let expected = variant == .seven ? witness.activations7 : witness.activations8
                // Float32 recurrent gates amplify different Apple/Eigen
                // reductions on the sustained pulse case. The observed bound
                // is 0.00031; the mean stays below 0.000003. Separate HMM
                // witnesses require identical decoded beat events and states.
                assertClose(activations, expected.floats, tolerance: 0.0005,
                            label: "\(witness.recipe.name) model\(variant.rawValue)")
                let meanError = zip(activations, expected.floats).reduce(Float(0)) { $0 + abs($1.0 - $1.1) } / Float(activations.count)
                XCTAssertLessThanOrEqual(meanError, 0.000005, "\(witness.recipe.name) model\(variant.rawValue) mean error")
            }
        }
    }

    private func assertClose(_ actual: [Float], _ expected: [Float], tolerance: Float, label: String,
                             file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(actual.count, expected.count, label, file: file, line: line)
        XCTAssertTrue(actual.allSatisfy(\.isFinite), label, file: file, line: line)
        let error = zip(actual, expected).map { abs($0 - $1) }.max() ?? 0
        XCTAssertLessThanOrEqual(error, tolerance, "\(label), maximum absolute error \(error)", file: file, line: line)
    }
}

private struct ModelWitnesses: Decodable {
    let sourceSHA256: String
    let cases: [Witness]
    struct Witness: Decodable {
        let recipe: Recipe
        let sampleSHA256: String
        let frameCount: Int
        let featureFrameIndices: [Int]
        let expectedFeatures: Data
        let activations7: Data
        let activations8: Data
    }
    struct Recipe: Decodable {
        let name: String
        let sampleCount: Int
        let noiseSeed: UInt32
        let noiseAmplitude: Int
        let pulses: [Int]

        func samples() -> [Float] {
            var state = noiseSeed, current = -1
            return (0..<sampleCount).map { index in
                state = 1_664_525 &* state &+ 1_013_904_223
                var value = Int(state >> 16) % (noiseAmplitude * 2 + 1) - noiseAmplitude
                if current + 1 < pulses.count, index == pulses[current + 1] { current += 1 }
                if current >= 0 {
                    let age = index - pulses[current]
                    if age < 1_536 {
                        value += (1_536 - age) * (current % 4 == 0 ? 12 : 6) * (age / 11 % 2 == 0 ? 1 : -1)
                    }
                }
                return Float(value) / 32_768
            }
        }
    }
}

private extension Data {
    var floats: [Float] { withUnsafeBytes { Array($0.bindMemory(to: Float.self)) } }
}
