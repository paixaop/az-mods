import Foundation
import XCTest
@testable import Analysis

final class NativeBeatExportTests: XCTestCase {
    func testSamplePositionsBarSequenceAndMedianMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatExport", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Witnesses.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for (index, witness) in fixture.cases.enumerated() {
            let actual = try NativeBeatExport.make(events: witness.events, sampleCount: witness.sampleCount,
                                                  sampleRate: witness.sampleRate)
            XCTAssertEqual(actual, witness.expected, "original witness \(index)")
        }
    }

    private struct Witnesses: Decodable {
        let sourceSHA256: String
        let cases: [Witness]
        struct Witness: Decodable {
            let events: [NativeBeatExport.Event]
            let sampleCount: Int
            let sampleRate: Int
            let expected: NativeBeatExport.Result
        }
    }
}
