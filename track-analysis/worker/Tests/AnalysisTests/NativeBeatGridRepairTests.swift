import Foundation
import XCTest
@testable import Analysis

final class NativeBeatGridRepairTests: XCTestCase {
    private struct Fixture: Decodable {
        var cases: [Case]
        struct Case: Decodable {
            var operation: String
            var sampleRate: Int
            var input: [[Double]]
            var output: [[Double]]?
            var first: Int?, last: Int?, sourceFrameCount: Int?
            var candidates: [[Double]]?
            var bpm: Float?, amplitude: Int?, seed: Int?, peakOutput: Bool?
        }
    }

    func testGridRepairsAndPeakDecisionMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatGridRepair", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: url))
        func records(_ rows: [[Double]]) -> [NativeBeatInfo] {
            rows.map { .init(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]), sourceBand: Int($0[4]), flag: UInt8($0[5])) }
        }
        for (index, item) in fixture.cases.enumerated() {
            var result = records(item.input)
            let context = "Case \(index), \(item.operation)"
            switch item.operation {
            case "span":
                result = NativeBeatGridRepair.correctSpan(result, sampleRate: item.sampleRate,
                    sourceFrameCount: try XCTUnwrap(item.sourceFrameCount), first: try XCTUnwrap(item.first),
                    last: try XCTUnwrap(item.last))
            case "gaps": NativeBeatGridRepair.correctGaps(&result)
            case "segments":
                try NativeBeatGridRepair.alignSegments(&result, candidates: records(try XCTUnwrap(item.candidates)),
                    sampleRate: item.sampleRate, sourceFrameCount: try XCTUnwrap(item.sourceFrameCount))
            case "peak":
                let count = try XCTUnwrap(item.sourceFrameCount), amplitude = try XCTUnwrap(item.amplitude)
                let seed = try XCTUnwrap(item.seed)
                let low = (0...(count / 4)).map { Int16(amplitude * (($0 * 37 + seed) % 257) / 256) }
                let actual = NativeBeatGridRepair.judgeDynamicPeak(candidates: result, low: low,
                    sampleRate: item.sampleRate, sourceFrameCount: count, globalBPM: try XCTUnwrap(item.bpm))
                XCTAssertEqual(actual, try XCTUnwrap(item.peakOutput), context)
                continue
            default: XCTFail("Unknown fixture operation: \(item.operation)"); continue
            }
            XCTAssertEqual(result, records(try XCTUnwrap(item.output)), context)
        }
    }
}
