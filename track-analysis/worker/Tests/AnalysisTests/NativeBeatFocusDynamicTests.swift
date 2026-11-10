import XCTest
@testable import Analysis

final class NativeBeatFocusDynamicTests: XCTestCase {
    private struct Fixture: Decodable { let cases: [Case] }
    private struct Case: Decodable {
        let events: [[Int]]
        let rate: Int
        let count: Int
        let modelBPM100: Int
        let expected: Expected
    }
    private struct Expected: Decodable {
        let rows: [[Double]]
        let bpm: Float
        let flag: Int
    }

    func testCompleteVariableTempoFocusMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFocusDynamic", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.cases.count, 220)
        for (index, sample) in fixture.cases.enumerated() {
            let model = sample.events.enumerated().map { i, row in
                NativeBeatExport.Beat(index: i, bpm: row[2], pos: row[0], msec: 0, numBar: row[1])
            }
            let result = try NativeBeatFocusDynamic.analyze(model: model,
                modelBPM100: sample.modelBPM100, sampleRate: sample.rate, sourceFrameCount: sample.count)
            XCTAssertEqual(result.bpm, sample.expected.bpm, "case \(index)")
            XCTAssertEqual(result.changedTempoOctave, sample.expected.flag != 0, "case \(index)")
            let rows = result.beats.map {
                [Double($0.sampleIndex), Double($0.intervalInBeats), Double($0.periodMilliseconds),
                 Double($0.beatInBar), Double($0.sourceBand), Double($0.flag)]
            }
            XCTAssertEqual(rows, sample.expected.rows, "case \(index)")
        }
    }
}
