import XCTest
@testable import Analysis

final class NativeBeatFocusDynamicRepairTests: XCTestCase {
    private struct Witness: Decodable {
        let rows: [[Double]]
        let expected: [[Double]]
        let ratios: [Double]
        let symmetricRatios: [Double]
        let first: Int
        let last: Int
        let sampleRate: Int
        let sourceFrameCount: Int
        let period: Double
        let subranges: [[Int]]
    }

    func testMetricalRateRepairsMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFocusDynamicRepair", withExtension: "json"))
        let witnesses = try JSONDecoder().decode([Witness].self, from: Data(contentsOf: url))
        for (index, witness) in witnesses.enumerated() {
            var beats = witness.rows.map {
                NativeBeatInfo(sampleIndex: Int($0[0]), intervalInBeats: Float($0[1]),
                    periodMilliseconds: Float($0[2]), beatInBar: Int($0[3]), sourceBand: Int($0[4]), flag: UInt8($0[5]))
            }
            let reset = try NativeBeatFocusDynamicRepair.apply(beats: &beats,
                first: witness.first, last: witness.last, ratios: witness.ratios,
                symmetricRatios: witness.symmetricRatios, period: witness.period,
                sampleRate: witness.sampleRate, sourceFrameCount: witness.sourceFrameCount,
                subranges: witness.subranges.map { .init(first: $0[0], last: $0[1]) })
            XCTAssertFalse(reset, "witness \(index)")
            let rows = beats.map {
                [Double($0.sampleIndex), Double($0.intervalInBeats), Double($0.periodMilliseconds),
                 Double($0.beatInBar), Double($0.sourceBand), Double($0.flag)]
            }
            XCTAssertEqual(rows, witness.expected, "witness \(index)")
        }
    }
}
