import Foundation
import XCTest
@testable import Analysis

final class NativeBeatAdjustTests: XCTestCase {
    private struct CompleteFixture: Decodable {
        let cases: [Case]
        struct Case: Decodable {
            let sampleRate: Int
            let sourceFrameCount: Int
            let bpm: Double
            let signalBPM: Double?
            let input: [[Double]]
            let outputBPM: Float
            let output: [[Double]]
        }
    }
    private struct AlignmentFixture: Decodable {
        let cases: [Case]
        struct Case: Decodable {
            let values: [Double]
            let allMaximum: Double
            let all: Selection
            let each: Selection
        }
        struct Selection: Decodable {
            let value: Double
            let position: Int
            let rejected: Bool?
        }
    }
    private struct PeakFixture: Decodable {
        let cases: [Case]
        struct Case: Decodable {
            let input: [Double]
            let peaks: [Peak]
        }
        struct Peak: Decodable {
            let position: Int
            let height: Double
            let prominence: Double
            let width: Double
        }
    }
    private struct Fixture: Decodable {
        let cases: [Case]
        struct Case: Decodable {
            let sampleRate: Int
            let sourceFrameCount: Int
            let pattern: String
            let frameCounts: [Int]
            let high: [Int16]
            let low: [Int16]
            let full: [Int16]
        }
    }

    func testStreamingEnvelopesMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatAdjust", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: url))
        for item in fixture.cases {
            let processor = NativeBeatAdjust(sampleRate: item.sampleRate, sourceFrameCount: item.sourceFrameCount)
            var position = 0
            for count in item.frameCounts {
                var pcm: [Int16] = []
                pcm.reserveCapacity(count * 2)
                for i in position..<(position + count) {
                    let left: Int, right: Int
                    if item.pattern == "impulse" {
                        left = i % 809 < 7 ? 30_000 : i % 997 < 13 ? -24_000 : 0
                        right = i % 599 < 11 ? -12_000 : i % 733 < 5 ? 21_000 : 0
                    } else {
                        left = (i * 977 + 53) % 65_536 - 32_768
                        right = item.pattern == "antiphase" && left != -32_768
                            ? -left : (i * 431 + 127) % 65_536 - 32_768
                    }
                    pcm.append(Int16(left)); pcm.append(Int16(right))
                }
                processor.append(interleaved: pcm, frameCount: count)
                position += count
            }
            for (name, actual, expected) in [("high", processor.high, item.high),
                                             ("low", processor.low, item.low),
                                             ("full", processor.full, item.full)] {
                let context = "\(item.sampleRate) \(item.pattern) \(name)"
                XCTAssertEqual(actual.count, expected.count, context)
                let mismatch = zip(actual, expected).enumerated().first { $0.element.0 != $0.element.1 }
                XCTAssertNil(mismatch, "\(context): first mismatch \(String(describing: mismatch))")
            }
        }
    }

    func testPeakProminenceAndWidthMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatPeaks", withExtension: "json"))
        let fixture = try JSONDecoder().decode(PeakFixture.self, from: Data(contentsOf: url))
        for (index, item) in fixture.cases.enumerated() {
            let actual = NativeBeatPeaks.find(item.input)
            XCTAssertEqual(actual.count, item.peaks.count, "Case \(index)")
            for (a, e) in zip(actual, item.peaks) {
                let context = "Case \(index), peak \(e.position)"
                XCTAssertEqual(a.position, e.position, context)
                XCTAssertEqual(a.height, e.height, context)
                XCTAssertEqual(a.prominence, e.prominence, context)
                XCTAssertEqual(a.width, e.width, accuracy: 1e-12, context)
            }
        }
    }

    func testOnsetAlignmentMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatAlignment", withExtension: "json"))
        let fixture = try JSONDecoder().decode(AlignmentFixture.self, from: Data(contentsOf: url))
        for (index, item) in fixture.cases.enumerated() {
            let all = NativeBeatAlignment.all(item.values)
            let each = NativeBeatAlignment.each(item.values, allMaximum: item.allMaximum)
            for (name, a, e) in [("all", all, item.all), ("each", each, item.each)] {
                let context = "Case \(index) \(name)"
                XCTAssertEqual(a.position, e.position, context)
                XCTAssertEqual(a.value, e.value, context)
                XCTAssertEqual(a.rejected, e.rejected ?? false, context)
            }
        }
    }

    func testCompleteTempoAndPositionAdjustmentMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatCompleteAdjust", withExtension: "json"))
        let fixture = try JSONDecoder().decode(CompleteFixture.self, from: Data(contentsOf: url))
        for item in fixture.cases {
            let rate = item.sampleRate
            let processor = NativeBeatAdjust(sampleRate: rate, sourceFrameCount: item.sourceFrameCount)
            let cadence = rate / 75, period = Int(Double(rate) * 60 / (item.signalBPM ?? item.bpm))
            for start in stride(from: 0, through: item.sourceFrameCount, by: cadence) {
                let count = min(cadence, item.sourceFrameCount + 1 - start)
                var pcm: [Int16] = []
                pcm.reserveCapacity(count * 2)
                for i in start..<(start + count) {
                    let phase = ((i - rate / 20) % period + period) % period
                    let amplitude = Int16(max(0, 12_000 - phase * 12))
                    pcm.append(i % 8 < 4 ? amplitude : -amplitude)
                    pcm.append(i % 12 < 6 ? amplitude : -amplitude)
                }
                processor.append(interleaved: pcm, frameCount: count)
            }
            func records(_ rows: [[Double]]) -> [NativeBeatInfo] {
                rows.map { .init(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                    periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]),
                    sourceBand: Int($0[4]), flag: UInt8($0[5])) }
            }
            var beats = records(item.input), tempo = Float(item.bpm)
            try processor.analyze(tempo: &tempo, beats: &beats)
            XCTAssertEqual(tempo, item.outputBPM, "Sample rate \(rate)")
            XCTAssertEqual(beats, records(item.output), "Sample rate \(rate)")
        }
    }

    func testZeroEnvelopeEvidenceDoesNotInventMinimumTempo() {
        let rate = 44_100, frames = rate * 2
        let processor = NativeBeatAdjust(sampleRate: rate, sourceFrameCount: frames - 1)
        let cadence = rate / 75
        for start in stride(from: 0, to: frames, by: cadence) {
            let count = min(cadence, frames - start)
            processor.append(interleaved: [Int16](repeating: 0, count: count * 2), frameCount: count)
        }
        let original = (0..<4).map { index in
            NativeBeatInfo(sampleIndex: index * rate / 2, intervalInBeats: 0,
                periodMilliseconds: 500, beatInBar: index + 1, sourceBand: -1, flag: 0)
        }
        var beats = original, tempo: Float = 120
        XCTAssertThrowsError(try processor.analyze(tempo: &tempo, beats: &beats)) { error in
            XCTAssertEqual(error as? NativeBeatAdjust.Failure, .noTempoEvidence)
        }
        XCTAssertEqual(tempo, 120)
        XCTAssertEqual(beats, original)
    }
}
