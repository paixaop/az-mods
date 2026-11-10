import Foundation
import XCTest
@testable import Analysis

final class NativeBeatDynamicsTests: XCTestCase {
    private struct Case: Decodable {
        var sampleRate: Int
        var constantBPM: Float, variableBPM: Float, peakDynamic: Bool
        var variableGrid: [[Double]]
        var expected: Expected
        struct Expected: Decodable {
            var timingFlags: [Int]
            var variableBPM: Float, constantBPM: Float
            var extraBool: Bool, selectVariable: Bool
        }
    }

    func testTimingDecisionsAndBothTemposMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatDynamics", withExtension: "json"))
        let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: url))
        XCTAssertEqual(cases.count, 348)
        for (index, item) in cases.enumerated() {
            let rows: [NativeBeatInfo] = item.variableGrid.map { .init(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]), sourceBand: Int($0[4]), flag: UInt8($0[5])) }
            let result = NativeBeatDynamics.select(variableGrid: rows, sampleRate: item.sampleRate,
                constantBPM: item.constantBPM, variableBPM: item.variableBPM, peakDynamic: item.peakDynamic)
            let context = "Case \(index)"
            XCTAssertEqual([result.jitterFlag, result.driftFlag, result.changeFlag].map { $0 ? 1 : 0 },
                           item.expected.timingFlags, context)
            XCTAssertEqual(result.refinedVariableBPM, item.expected.variableBPM, context)
            XCTAssertEqual(result.refinedConstantBPM, item.expected.constantBPM, context)
            XCTAssertEqual(result.extraBool, item.expected.extraBool, context)
            XCTAssertEqual(result.selectVariable, item.expected.selectVariable, context)
        }
    }
}
