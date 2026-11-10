import Foundation
import XCTest
@testable import Analysis

final class NativeBeatInfoTests: XCTestCase {
    private struct Case: Decodable {
        let pattern: String?
        let input: [[Double]]
        let sampleRate: Int
        let sourceFrameCount: Int
        let expected: [[Double]]
        let globalBPM: Float
        let detectedDynamic: Bool
        let extraBool: Bool
    }

    func testCompleteGridSelectionMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatInfo", withExtension: "json"))
        let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: url))
        // Gradually changing periods must update the comparison reference in
        // both group pruning and record pruning. These original executions cover
        // rising, falling, returning and briefly interrupted period sequences.
        XCTAssertEqual(cases.filter { $0.pattern?.hasPrefix("rolling-") == true }.count, 4)
        for (index, item) in cases.enumerated() {
            let label = item.pattern ?? "case \(index)"
            let candidates = item.input.map {
                NativeBeatInfo(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                    periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]), sourceBand: Int($0[4]), flag: UInt8($0[5]))
            }
            let result = try NativeBeatInfoAnalyzer.analyze(candidates: candidates,
                low: [Int16](repeating: 0, count: item.sourceFrameCount / 4 + 100),
                sampleRate: item.sampleRate, sourceFrameCount: item.sourceFrameCount)
            let actual = result.beats.map {
                [Double($0.sampleIndex), Double($0.intervalInBeats), Double($0.periodMilliseconds),
                 Double($0.beatInBar), Double($0.sourceBand), Double($0.flag)]
            }
            XCTAssertEqual(actual, item.expected, label)
            XCTAssertEqual(result.globalBPM, item.globalBPM, label)
            XCTAssertEqual(result.detectedDynamic, item.detectedDynamic, label)
            XCTAssertEqual(result.extraBool, item.extraBool, label)
        }
    }
}
