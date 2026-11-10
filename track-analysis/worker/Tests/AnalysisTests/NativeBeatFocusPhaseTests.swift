import Foundation
import XCTest
@testable import Analysis

final class NativeBeatFocusPhaseTests: XCTestCase {
    private struct Call: Decodable, Equatable {
        let kind: String
        let position: Int
        let bpm: Float
    }
    private struct PhaseCase: Decodable {
        let name: String
        let sampleRate: Int
        let bpm: Float
        let beats: [[Double]]
        let low: [Double]
        let full: [Double]
        let expectedPhases: [Int]
        let expectedIntegralCalls: [Call]
    }
    private struct CompositionCase: Decodable {
        struct Expected: Decodable {
            let bpm: Float
            let flag: Int
            let rows: [[Double]]
        }
        let name: String
        let events: [[Int]]
        let rate: Int
        let count: Int
        let legacyBPM100: Int
        let offset: Int
        let energyMode: Int
        let expected: Expected
    }
    private struct Fixture: Decodable {
        let sourceSHA256: String
        let cases: [PhaseCase]
        let composition: [CompositionCase]
    }

    private func fixture() throws -> Fixture {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFocusPhase", withExtension: "json"))
        return try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: url))
    }

    func testPhaseVotesAndIntegralArgumentsMatchOriginalExecution() throws {
        let fixture = try fixture()
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        XCTAssertEqual(fixture.cases.count, 64)
        for sample in fixture.cases {
            var beats = sample.beats.map(beat)
            let positions = beats.map(\.sampleIndex)
            var calls: [Call] = []
            func integral(_ kind: String, _ bpm: Float, _ position: Int) -> Double {
                calls.append(Call(kind: kind, position: position, bpm: bpm))
                let index = positions.firstIndex(of: position).map { $0 + 1 } ?? 0
                return (kind == "low" ? sample.low : sample.full)[index]
            }
            try NativeBeatFocusPhase.postprocess(beats: &beats, sampleRate: sample.sampleRate,
                bpm: sample.bpm, lowIntegral: { integral("low", $0, $1) },
                fullIntegral: { integral("full", $0, $1) })
            var expected = sample.beats.map(beat)
            XCTAssertEqual(expected.count, sample.expectedPhases.count, sample.name)
            for i in expected.indices { expected[i].beatInBar = sample.expectedPhases[i] }
            XCTAssertEqual(beats, expected, sample.name)
            XCTAssertEqual(calls, sample.expectedIntegralCalls, sample.name)
        }
    }

    func testNormalFocusAndPhaseCompositionMatchesOriginalExecution() throws {
        let cases = try fixture().composition
        XCTAssertEqual(cases.count, 32)
        for sample in cases {
            let model = sample.events.enumerated().map { index, row in
                NativeBeatExport.Beat(index: index, bpm: row[2], pos: row[0], msec: 0, numBar: row[1])
            }
            let lowIntegral: (Float, Int) -> Double = { _, position in
                sample.energyMode == 0 ? 0 : Double(max(0, abs(position / 73) % 1000 - 300))
            }
            var result = try NativeBeatFocus.normal(model: model, legacyBPM100: sample.legacyBPM100,
                sampleRate: sample.rate, sourceFrameCount: sample.count, windowOffset: sample.offset,
                lowIntegral: lowIntegral)
            // The original composition witness uses an identity position-adjust
            // dependency and the same explicit integral functions for both phases.
            try NativeBeatFocusPhase.postprocess(beats: &result.beats, sampleRate: sample.rate,
                bpm: result.bpm, lowIntegral: lowIntegral, fullIntegral: { _, _ in 0 })
            XCTAssertEqual(result.bpm, sample.expected.bpm, sample.name)
            XCTAssertEqual(result.changedTempoOctave, sample.expected.flag != 0, sample.name)
            XCTAssertEqual(result.beats, sample.expected.rows.map(beat), sample.name)
        }
    }

    func testShortGridDoesNotReadOutsideItsPhaseRanking() {
        var beats = (0..<13).map {
            NativeBeatInfo(sampleIndex: $0 * 22_050, intervalInBeats: 1,
                           periodMilliseconds: 500, beatInBar: $0 % 4 + 1, sourceBand: 0, flag: 0)
        }
        XCTAssertThrowsError(try NativeBeatFocusPhase.postprocess(beats: &beats, sampleRate: 44_100,
            bpm: 120, lowIntegral: { _, _ in XCTFail("No integral is valid for this grid"); return 0 },
            fullIntegral: { _, _ in XCTFail("No integral is valid for this grid"); return 0 })) {
                guard case NativeBeatFocusPhase.Failure.insufficientBeats = $0 else {
                    return XCTFail("Unexpected failure: \($0)")
                }
            }
    }

    private func beat(_ row: [Double]) -> NativeBeatInfo {
        .init(sampleIndex: Int(row[0]), intervalInBeats: Float(row[1]),
              periodMilliseconds: Float(row[2]), beatInBar: Int(row[3]),
              sourceBand: Int(row[4]), flag: UInt8(row[5]))
    }
}
