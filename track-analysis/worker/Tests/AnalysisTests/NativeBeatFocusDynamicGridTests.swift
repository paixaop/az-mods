import Foundation
import XCTest
@testable import Analysis

final class NativeBeatFocusDynamicGridTests: XCTestCase {
    private struct Witness: Decodable {
        struct Expected: Decodable { let beats: [[Double]]; let bpm: Float }
        let id: String
        let sampleRate: Int
        let sourceFrameCount: Int
        let beats: [[Double]]
        let anchorSample: Int
        let anchorBar: Int
        let expected: Expected
    }

    func testDynamicGridMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFocusDynamicGrid", withExtension: "json"))
        let cases = try JSONDecoder().decode([Witness].self, from: Data(contentsOf: url))
        for witness in cases {
            let result = try NativeBeatFocusDynamicGrid.finish(beats: records(witness.beats),
                sampleRate: witness.sampleRate, sourceFrameCount: witness.sourceFrameCount,
                anchorSample: witness.anchorSample, anchorBar: witness.anchorBar)
            XCTAssertEqual(result.beats, records(witness.expected.beats), witness.id)
            XCTAssertEqual(result.bpm, witness.expected.bpm, witness.id)
            XCTAssertFalse(result.changedTempoOctave, witness.id)
        }
    }

    private func records(_ rows: [[Double]]) -> [NativeBeatInfo] {
        rows.map { .init(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                        periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]),
                        sourceBand: Int($0[4]), flag: UInt8($0[5])) }
    }
}
