import XCTest
@testable import Analysis

final class NativeBeatFocusTests: XCTestCase {
    func testShortGridRetainsRemovedTailForStableTimingAverage() throws {
        // A twelve-second synthetic click recording produces 26 model events.
        // Repair reduces its active grid to 25 while the stable interval still
        // includes the final source event. Expected positions come from original
        // execution before audio adjustment, with low-band integrals set to zero.
        let positions = [9702, 30429, 51156, 71442, 92169, 112896, 133623,
            154350, 175077, 195804, 216531, 237258, 257544, 278271,
            298998, 319725, 340452, 361179, 381906, 402192, 422919,
            443646, 464373, 485100, 505827, 526554]
        let model = positions.enumerated().map { index, position in
            NativeBeatExport.Beat(index: index, bpm: [2, 11, 18].contains(index) ? 13043 : 12765,
                pos: position, msec: 0, numBar: (index + 3) % 4 + 1)
        }
        let result = try NativeBeatFocus.normal(model: model, legacyBPM100: 12800,
            sampleRate: 44100, sourceFrameCount: 529263, windowOffset: 0,
            lowIntegral: { _, _ in 0 })
        XCTAssertEqual(result.bpm, 128)
        XCTAssertFalse(result.changedTempoOctave)
        XCTAssertEqual(result.beats.map(\.sampleIndex), [9509, 30181, 50853, 71524,
            92196, 112868, 133540, 154212, 174884, 195556, 216228, 236899,
            257571, 278243, 298915, 319587, 340259, 360931, 381603, 402274,
            422946, 443618, 464290, 484962, 505634, 526306])
        for (index, beat) in result.beats.enumerated() {
            XCTAssertEqual(beat.periodMilliseconds, 468.75)
            XCTAssertEqual(beat.beatInBar, (index + 3) % 4 + 1)
            XCTAssertEqual(beat.intervalInBeats, 0)
            XCTAssertEqual(beat.sourceBand, -1)
            XCTAssertEqual(beat.flag, 0)
        }
    }

    private struct Fixture: Decodable { let cases: [Case] }
    private struct Case: Decodable {
        let events: [[Int]]
        let rate: Int
        let count: Int
        let legacyBPM100: Int
        let offset: Int
        let energyMode: Int
        let expected: Expected
    }
    private struct Expected: Decodable {
        let bpm: Float
        let flag: Int
        let rows: [[Double]]
    }

    func testNormalFocusMatchesOriginalExecutions() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFocus", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.cases.count, 400)
        for (index, sample) in fixture.cases.enumerated() {
            let model = sample.events.enumerated().map { i, row in
                NativeBeatExport.Beat(index: i, bpm: row[2], pos: row[0], msec: 0, numBar: row[1])
            }
            let result = try NativeBeatFocus.normal(model: model, legacyBPM100: sample.legacyBPM100,
                sampleRate: sample.rate, sourceFrameCount: sample.count, windowOffset: sample.offset,
                lowIntegral: { _, position in
                    sample.energyMode == 0 ? 0 : Double(max(0, abs(position / 73) % 1000 - 300))
                })
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
