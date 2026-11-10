import Foundation
import XCTest
@testable import Analysis

final class NativePhraseRouteTests: XCTestCase {
    func testProfilesAndRouteMatchOriginalNativeExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseRoute", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for (index, row) in (root["route"] as! [[String: Any]]).enumerated() {
            let mood = row["mood"] as! Int
            let result = try NativePhraseRoute.classify(lowPeaks: row["low"] as! [Double],
                phase: row["phase"] as! Int, duration: row["duration"] as! Double,
                loopTypes: row["loop"] as! [Int], rhythmTypes: row["rhythm"] as! [Int],
                mood: mood == 1 ? .high : mood == 2 ? .mid : .low)
            let profiles = row["profiles"] as! [[Double]]
            for (actual, expected) in [(result.positiveProfile, profiles[1]), (result.negativeProfile, profiles[2])] {
                XCTAssertEqual(actual.count, expected.count, "case \(index)")
                for (a, e) in zip(actual, expected) { XCTAssertEqual(a, e, accuracy: 1e-12, "case \(index)") }
            }
            XCTAssertEqual(result.mood == .high ? 1 : result.mood == .mid ? 2 : 3, row["expectedMood"] as! Int, "case \(index)")
            XCTAssertEqual(result.route, row["expectedRoute"] as! Int, "case \(index)")
        }
    }
}
