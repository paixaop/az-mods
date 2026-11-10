import XCTest
@testable import Analysis

final class NativePhraseHighClassifierTests: XCTestCase {
    private struct Witness: Decodable {
        let boundaries: [Int]
        let intro: [Double]
        let low: [Double]
        let roles: [Int]
        let simple: Bool
    }

    // Synthetic numerical vectors evaluated by the unchanged ARM64 function.
    // These expectations do not come from this Swift implementation or music labels.
    func testNativeExecutionWitnesses() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseHigh", withExtension: "json"))
        let cases = try JSONDecoder().decode([Witness].self, from: Data(contentsOf: url))
        for (index, witness) in cases.enumerated() {
            let actual = NativePhraseHighClassifier.classify(boundaries: witness.boundaries,
                introMeans: witness.intro, lowMeans: witness.low)
            XCTAssertEqual(actual.roles, witness.roles, "native witness \(index)")
            XCTAssertEqual(actual.simpleAlternation, witness.simple, "native witness \(index)")
        }
    }

    func testNativeShortDefaults() {
        XCTAssertEqual(NativePhraseHighClassifier.classify(boundaries: [1], introMeans: [0.1], lowMeans: [0.1]).roles, [10])
        XCTAssertEqual(NativePhraseHighClassifier.classify(boundaries: [1, 33], introMeans: [0.1, 0.2], lowMeans: [0.2, 0.1]).roles, [10, 60])
    }
}
