import Foundation
import XCTest
@testable import Analysis

final class NativeBeatGridTests: XCTestCase {
    private func witnesses() throws -> Witnesses {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatGrid", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Witnesses.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        return fixture
    }

    func testTempoScalingAndBarNumbersMatchOriginalExecution() throws {
        for (index, witness) in try witnesses().transforms.enumerated() {
            let actual: [NativeBeatGrid.DetailedBeat]
            switch witness.operation {
            case "double":
                actual = NativeBeatGrid.doubled(witness.beats, durationMicroseconds: witness.durationMicroseconds)
            case "half":
                actual = NativeBeatGrid.halved(witness.beats, durationMicroseconds: witness.durationMicroseconds)
            case "renumber":
                actual = NativeBeatGrid.renumbered(witness.beats, firstPosition: witness.firstPosition,
                    durationMicroseconds: witness.durationMicroseconds)
            default:
                XCTFail("Unknown original witness operation: \(witness.operation)")
                continue
            }
            XCTAssertEqual(actual, witness.expected, "\(witness.operation) witness \(index)")
        }
    }

    func testSampleClockAndTempoRangeMatchOriginalExecution() throws {
        for (index, witness) in try witnesses().conversions.enumerated() {
            let actual = try NativeBeatGrid.convert(samples: witness.samples, sampleRate: witness.sampleRate,
                sampleCount: witness.sampleCount, globalBPM100: witness.globalBPM100,
                rangeIndex: witness.rangeIndex, dynamic: witness.dynamic)
            XCTAssertEqual(actual, witness.expected, "conversion witness \(index)")
        }
    }

    func testFirstBeatOffsetAndPhaseMatchOriginalExecution() throws {
        for (index, witness) in try witnesses().firstBeats.enumerated() {
            let actual = NativeBeatGrid.firstBeat(witness.beats, globalBPM100: witness.globalBPM100, initial: witness.initial)
            XCTAssertEqual(actual, witness.expected, "first-beat witness \(index)")
        }
    }
}

private struct Witnesses: Decodable {
    var sourceSHA256: String
    struct Transform: Decodable {
        var operation: String
        var beats: [NativeBeatGrid.DetailedBeat]
        var durationMicroseconds: Int64
        var firstPosition: Int
        var expected: [NativeBeatGrid.DetailedBeat]
    }
    struct Conversion: Decodable {
        var samples: [NativeBeatGrid.SampleBeat]
        var sampleRate: Int
        var sampleCount: Int
        var globalBPM100: Int
        var rangeIndex: Int
        var dynamic: Bool
        var expected: NativeBeatGrid.Result
    }
    struct First: Decodable {
        var beats: [NativeBeatGrid.DetailedBeat]
        var globalBPM100: Int
        var initial: NativeBeatGrid.FirstBeat
        var expected: NativeBeatGrid.FirstBeat
    }
    var transforms: [Transform]
    var conversions: [Conversion]
    var firstBeats: [First]
}
