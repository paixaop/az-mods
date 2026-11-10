#if canImport(CryptoKit)
import CryptoKit
#endif
import Foundation
import XCTest
@testable import Analysis

final class NativeBeatHMMTests: XCTestCase {
    private func witnesses() throws -> HMMWitnesses {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatHMM", withExtension: "json"))
        return try JSONDecoder().decode(HMMWitnesses.self, from: Data(contentsOf: url))
    }

    func testRegeneratedStateGraphMatchesEveryOriginalValue() throws {
        let fixture = try witnesses()
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        let model = try NativeBeatHMM.makeModel(beatsPerBar: 4, configuration: .native)
        let arrays: [String: Data] = [
            "positions": packed(model.positions.map { $0.bitPattern.littleEndian }),
            "intervals": packed(model.intervals.map { UInt32($0).littleEndian }),
            "observations": packed(model.observations.map { UInt32($0).littleEndian }),
            "previous": packed(model.previous.map { UInt32($0).littleEndian }),
            "pointers": packed(model.pointers.map { UInt32($0).littleEndian }),
            "probabilities": packed(model.probabilities.map { $0.bitPattern.littleEndian })
        ]
        for (name, data) in arrays {
            let expected = try XCTUnwrap(fixture.model[name])
            let width = name == "positions" || name == "probabilities" ? 8 : 4
            XCTAssertEqual(data.count / width, expected.count, name)
            let digest = try fixtureSHA256(data)
            XCTAssertEqual(digest, expected.sha256, name)
        }
    }

    func testBeatEventsMatchOriginalExecution() throws {
        for witness in try witnesses().cases {
            let actual = try NativeBeatHMM.track(activations: input(witness), configuration: witness.settings)
            XCTAssertEqual(actual, witness.expected, witness.name)
        }
    }

    func testCompressedTracebackAndScoresMatchOriginalExecution() throws {
        let model = try NativeBeatHMM.makeModel(beatsPerBar: 4, configuration: .native)
        for witness in try witnesses().cases {
            guard let expected = witness.path else { continue }
            let activations = try input(witness)
            let first = expected.firstFrame * 2
            let end = first + expected.states.count * 2
            let cropped = Array(activations[first..<end])
            let densities = NativeBeatHMM.logDensities(cropped, observationLambda: 16)
            let actual = try NativeBeatHMM.viterbi(densities: densities, model: model)
            XCTAssertEqual(actual.states, expected.states, witness.name)
            XCTAssertEqual(actual.logProbability, expected.logProbability, accuracy: 1e-9, witness.name)
        }
    }

    func testTrackerCooperatesWithCancellation() async {
        let task = Task {
            try NativeBeatHMM.track(activations: [Float](repeating: 0.1, count: 12_000))
        }
        task.cancel()
        do {
            _ = try await task.value
            XCTFail("A cancelled timing analysis completed")
        } catch is CancellationError {
        } catch {
            XCTFail("Unexpected cancellation error: \(error)")
        }
    }

    private func packed<T>(_ values: [T]) -> Data {
        values.withUnsafeBytes { Data($0) }
    }

    private func input(_ witness: HMMWitnesses.Witness) throws -> [Float] {
        if let activations = witness.activations { return activations }
        let name = try XCTUnwrap(witness.modelCase)
        let model = try XCTUnwrap(witness.model)
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatModel", withExtension: "json"))
        let fixture = try JSONDecoder().decode(ModelActivations.self, from: Data(contentsOf: url))
        let source = try XCTUnwrap(fixture.cases.first(where: { $0.recipe.name == name }))
        let data = try XCTUnwrap(Data(base64Encoded: model == 7 ? source.activations7 : source.activations8))
        return data.withUnsafeBytes { bytes in
            stride(from: 0, to: bytes.count, by: 4).map {
                Float(bitPattern: UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: $0, as: UInt32.self)))
            }
        }
    }
}

private struct HMMWitnesses: Decodable {
    struct Digest: Decodable {
        let count: Int
        let sha256: String
    }
    struct Witness: Decodable {
        struct Settings: Decodable {
            let correct: Bool?
            let threshold: Float?
        }
        struct Path: Decodable {
            let firstFrame: Int
            let states: [Int]
            let logProbability: Double
        }
        let name: String
        let activations: [Float]?
        let modelCase: String?
        let model: Int?
        let configuration: Settings
        let expected: [NativeBeatHMM.Event]
        let path: Path?

        var settings: NativeBeatHMM.Configuration {
            var result = NativeBeatHMM.Configuration.native
            if let correct = configuration.correct { result.correct = correct }
            if let threshold = configuration.threshold { result.threshold = threshold }
            return result
        }
    }
    let sourceSHA256: String
    let model: [String: Digest]
    let cases: [Witness]
}

private struct ModelActivations: Decodable {
    struct Case: Decodable {
        struct Recipe: Decodable { let name: String }
        let recipe: Recipe
        let activations7: String
        let activations8: String
    }
    let cases: [Case]
}
