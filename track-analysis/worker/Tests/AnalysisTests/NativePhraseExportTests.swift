import Foundation
import XCTest
@testable import Analysis

final class NativePhraseExportTests: XCTestCase {
    func testDetailedGridClampingOmitsCollapsedPhraseAndFill() throws {
        let entries = [
            ANLZPhraseEntry(index: 1, startBeat: 1, code: .init(kind: 1), b: 0,
                beat2: 0, beat3: 0, beat4: 0, fill: 1, fillStartBeat: 8),
            ANLZPhraseEntry(index: 2, startBeat: 9, code: .init(kind: 1), b: 0,
                beat2: 0, beat3: 0, beat4: 0, fill: 0, fillStartBeat: 0)
        ]
        let output = NativePhraseExport.Result(mood: .high, endBeat: 12, entries: entries,
            variation: .init(flag: 0, summary: 0), converterFlag: 1, auxiliary: [])
        let grid = (0..<8).map { Double($0) * 0.5 }
        let clock: (Int) -> Double? = { grid[min(max(0, $0 - 1), grid.count - 1)] }
        let phrases = try output.phrases(confidence: 0.75, omitCollapsedIntervals: true, sampleRate: 1000, timeAtBeat: clock)
        XCTAssertEqual(phrases.count, 1)
        XCTAssertEqual(phrases.first?.start, 0)
        XCTAssertEqual(phrases.first?.end, 3.5)
        XCTAssertNil(phrases.first?.fillIn)
        XCTAssertThrowsError(try output.phrases(confidence: 0.75, sampleRate: 1000, timeAtBeat: clock))
    }

    func testNonHighCompositionMatchesOriginalNativeExecutions() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseNonHigh", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for (i, row) in (root["nonHigh"] as! [[String: Any]]).enumerated() {
            let result = try NativePhraseExport.nonHigh(mood: row["mood"] as! Int == 2 ? .mid : .low,
                boundaries: row["boundaries"] as! [Int], recurrenceSpectra: row["recurrenceSpectra"] as! [[Double]],
                phase: row["phase"] as! Int, endBeat: row["endBeat"] as! Int,
                middleLevels: row["middleLevels"] as! [Double], openingEnvelope: row["openingEnvelope"] as! [Double],
                vocalActivity: row["vocalActivity"] as! [Double], lowPeaks: row["lowPeaks"] as! [Double],
                activity: (row["activity"] as! [Int]).map(UInt8.init), lowQuarters: row["lowQuarters"] as! [Double],
                powerQuarters: row["powerQuarters"] as! [Double], converterParameter: 0)
            XCTAssertEqual(result.entries.map { $0.code.kind }, row["expectedKinds"] as! [Int], "case \(i)")
            XCTAssertEqual(result.endBeat, row["expectedTerminal"] as! Int, "case \(i)")
            XCTAssertEqual(result.variation.flag, row["variationFlag"] as! Int, "case \(i)")
            XCTAssertEqual(result.variation.summary, row["variationSummary"] as! Double, accuracy: 1e-12, "case \(i)")
            XCTAssertTrue(result.entries.allSatisfy { $0.fill == 0 && $0.fillStartBeat == 0 }, "case \(i)")
        }
    }

    func testVariationMatchesOriginalNativeExecution() throws {
        for (index, row) in (try fixture()["variation"] as! [[String: Any]]).enumerated() {
            let result = NativePhraseExport.variation(envelope: row["envelope"] as! [Double], boundaries: row["boundaries"] as! [Int])
            XCTAssertEqual(result.flag, row["flag"] as! Int, "case \(index)")
            XCTAssertEqual(result.summary, row["summary"] as! Double, accuracy: 1e-12, "case \(index)")
        }
    }

    func testAllConvertedFieldsMatchOriginalNativeExecution() throws {
        for (index, row) in (try fixture()["conversion"] as! [[String: Any]]).enumerated() {
            let up = row["up"] as! [[Int]], mood = row["mood"] as! Int
            let result = NativePhraseExport.fields(mood: mood == 1 ? .high : mood == 2 ? .mid : .low,
                roles: row["roles"] as! [Int], auxiliary: row["auxiliary"] as! [Int],
                upFirst: up[0], upSecond: up[1], upThird: up[2], upStarts: up[3])
            let fields = row["fields"] as! [[Int]]
            let actual = [result.kind, result.k1, result.k2, result.b, result.beat2, result.beat3, result.beat4, result.k3, result.fill]
            for (column, pair) in zip(actual, fields[1...9]).enumerated() {
                XCTAssertEqual(pair.0, pair.1, "case \(index), column \(column)")
            }
            XCTAssertEqual(result.auxiliary, row["expectedAuxiliary"] as! [Int], "case \(index)")
        }
    }

    private func fixture() throws -> [String: Any] {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseExport", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        return root
    }
}
