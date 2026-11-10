import Foundation
import XCTest
@testable import Analysis

final class NativeBeatEnvelopeTests: XCTestCase {
    private func witnesses() throws -> EnvelopeWitnesses {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatEnvelope", withExtension: "json"))
        return try JSONDecoder().decode(EnvelopeWitnesses.self, from: Data(contentsOf: url))
    }

    func testStreamingEnvelopeAndBarPhaseMatchOriginalExecution() throws {
        let fixture = try witnesses()
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for witness in fixture.streaming {
            let envelope = NativeBeatEnvelope(sampleRate: witness.sampleRate, sourceFrameCount: witness.sourceFrameCount)
            var position = 0
            for count in witness.splits {
                let samples = pcm(pattern: witness.pattern, rate: witness.sampleRate, range: position..<(position + count))
                envelope.append(interleaved: samples, frameCount: count)
                position += count
            }
            XCTAssertEqual(position, witness.sourceFrameCount, witness.name)
            XCTAssertEqual(envelope.values, witness.expected, witness.name)
            if let input = witness.beats, let expected = witness.expectedBeats {
                var beats = input.map(\.value)
                envelope.analyze(globalBPM: try XCTUnwrap(witness.globalBPM), beats: &beats,
                                 dynamic: try XCTUnwrap(witness.dynamic))
                XCTAssertEqual(beats, expected.map(\.value), witness.name)
            }
        }
    }

    func testBarPhaseAndEndpointEditsMatchOriginalExecution() throws {
        for witness in try witnesses().analysis {
            var beats = witness.beats.map(\.value)
            NativeBeatEnvelope.analyze(values: witness.values, sampleRate: witness.sampleRate,
                                       sourceFrameCount: witness.sourceFrameCount, globalBPM: witness.globalBPM,
                                       beats: &beats, dynamic: witness.dynamic)
            XCTAssertEqual(beats, witness.expected.map(\.value), witness.name)
        }
    }

    private func pcm(pattern: Int, rate: Int, range: Range<Int>) -> [Int16] {
        range.flatMap { index -> [Int16] in
            switch pattern {
            case 0:
                return [Int16((index * 977 + 53) % 65_536 - 32_768),
                        Int16((index * 431 + 127) % 65_536 - 32_768)]
            case 1:
                let left = index % 809 < 7 ? 30_000 : index % 997 < 13 ? -24_000 : 0
                let right = index % 599 < 11 ? -12_000 : index % 733 < 5 ? 21_000 : 0
                return [Int16(left), Int16(right)]
            default:
                let step = index % (rate / 2), bar = (index / (rate / 2)) % 4
                let amplitude = bar == 0 ? 28_000 : bar == 2 ? 18_000 : 9_000
                let left = step < 1_500 ? ((step / 80) % 2 == 0 ? amplitude : -amplitude) : (index * 97) % 200 - 100
                return [Int16(left), Int16(left / 2)]
            }
        }
    }
}

private struct EnvelopeWitnesses: Decodable {
    struct Beat: Decodable {
        let sampleIndex: Int
        let intervalInBeats: Float
        let periodMilliseconds: Float
        let beatInBar: Int
        let sourceBand: Int
        let flag: UInt8

        var value: NativeBeatInfo {
            .init(sampleIndex: sampleIndex, intervalInBeats: intervalInBeats,
                  periodMilliseconds: periodMilliseconds, beatInBar: beatInBar,
                  sourceBand: sourceBand, flag: flag)
        }
    }
    struct Stream: Decodable {
        let name: String
        let sampleRate: Int
        let sourceFrameCount: Int
        let pattern: Int
        let splits: [Int]
        let expected: [Int16]
        let globalBPM: Float?
        let dynamic: Bool?
        let beats: [Beat]?
        let expectedBeats: [Beat]?
    }
    struct Analysis: Decodable {
        let name: String
        let sampleRate: Int
        let sourceFrameCount: Int
        let values: [Int16]
        let globalBPM: Float
        let beats: [Beat]
        let dynamic: Bool
        let expected: [Beat]
    }
    let sourceSHA256: String
    let streaming: [Stream]
    let analysis: [Analysis]
}
