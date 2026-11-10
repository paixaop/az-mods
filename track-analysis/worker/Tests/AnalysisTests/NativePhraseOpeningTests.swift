import Foundation
import XCTest
@testable import Analysis

final class NativePhraseOpeningTests: XCTestCase {
    func testFourHypothesesAndOpeningSelectionMatchOriginalNativeExecution() throws {
        let records = try fixture()
        for (i, row) in (records["judge"] as! [[String: Any]]).enumerated() {
            let result = try NativePhraseOpening.judge(envelope: row["envelope"] as! [Double],
                low: row["low"] as! [Double], middle: row["middle"] as! [Double],
                vocal: row["vocal"] as! [Double], phase: row["phase"] as! Int)
            XCTAssertEqual(result.barHypotheses, row["hypotheses"] as! [Int], "case \(i)")
            XCTAssertEqual(result.hypotheses, row["expected"] as! [Int], "case \(i)")
            XCTAssertEqual(result.selected, row["selected"] as! Int, "case \(i)")
            XCTAssertEqual(result.hasOpening, row["flag"] as! Int == 1, "case \(i)")
        }
    }

    func testOpeningWrapperIncludingShortTracksAndAnchorSelectionMatchesOriginalNativeExecution() throws {
        for (i, row) in (try fixture()["main"] as! [[String: Any]]).enumerated() {
            let result = try NativePhraseOpening.main(envelope: row["envelope"] as! [Double],
                low: row["low"] as! [Double], middle: row["middle"] as! [Double],
                vocal: row["vocal"] as! [Double], phase: row["phase"] as! Int, anchors: row["anchors"] as! [Int])
            XCTAssertEqual(result.hypotheses, row["expected"] as! [Int], "case \(i)")
            XCTAssertEqual(result.selected, row["selected"] as! Int, "case \(i)")
            XCTAssertEqual(result.hasOpening, row["flag"] as! Int == 1, "case \(i)")
        }
    }

    private func fixture() throws -> [String: Any] {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseOpening", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        return root
    }
}
