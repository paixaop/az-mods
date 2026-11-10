import Foundation
import XCTest
@testable import Analysis

final class NativePhraseClassifierTests: XCTestCase {
    // Expected values were produced by executing the original ARM64 functions
    // on these synthetic inputs. The fixture contains no music or executable.
    private func records(_ key: String) throws -> [[String: Any]] {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseClassifier", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        return try XCTUnwrap(root[key] as? [[String: Any]])
    }

    func testInitialMoodAndBPMMatchNativeExecution() throws {
        for (i, record) in try records("mood").enumerated() {
            let x = record["input"] as! [String: Any]
            var input = NativePhraseClassifier.MoodInputs()
            input.slowRock = x["slowRock"] as! Int; input.slowTempo = x["slowTempo"] as! Int
            input.lowPowerRatio = x["lowPowerRatio"] as! Double
            input.popRock = x["popRock"] as! Int; input.alternateRock = x["alternateRock"] as! Int
            input.sparseKick = x["sparseKick"] as! Int; input.fourKick = x["fourKick"] as! Int
            input.fadeOut = x["fadeOut"] as! Int; input.firstOverride = x["firstOverride"] as! Int
            input.secondOverride = x["secondOverride"] as! Int
            input.narrowBPM = x["narrowBPM"] as! Int; input.broadBPM = x["broadBPM"] as! Int
            input.typicalGenre = x["typicalGenre"] as! Int; input.compression = x["compression"] as! Double
            let result = NativePhraseClassifier.initialMood(input)
            XCTAssertEqual(result == .high ? 1 : result == .mid ? 2 : 3, record["expected"] as! Int, "case \(i)")
        }
        for record in try records("bpm") {
            let result = NativePhraseClassifier.bpmFlags(record["bpm"] as! Double)
            XCTAssertEqual([result.narrow, result.broad], record["expected"] as! [Int])
        }
    }

    func testRecurrenceMatchesNativeExecutionIncludingCenteredEqualRows() throws {
        for (i, record) in try records("recurrence").enumerated() {
            let result = try NativePhraseClassifier.recurrence(spectra: record["spectra"] as! [[Double]],
                boundaries: record["boundaries"] as! [Int], phase: record["phase"] as! Int, endBeat: record["endBeat"] as! Int)
            let expected = record["expected"] as! [[Int]]
            for a in expected.indices {
                for b in (a + 1)..<expected.count {
                    XCTAssertEqual(result.related[a].contains(b), expected[a][b] == 1, "case \(i), pair \(a),\(b)")
                }
            }
        }
    }

    func testNonHighSeedsAndOrderedReconciliationMatchNativeExecution() throws {
        for (i, record) in try records("seeds").enumerated() {
            let result = try NativePhraseClassifier.nonHighSeeds(middleLevels: record["middleLevels"] as! [Double],
                openingEnvelope: record["openingEnvelope"] as! [Double], boundaries: record["boundaries"] as! [Int],
                vocalActivity: record["vocalActivity"] as! [Double], mood: record["mood"] as! Int == 2 ? .mid : .low)
            XCTAssertEqual(result.roles, record["expected"] as! [Int], "case \(i)")
        }
        for (i, record) in try records("roles").enumerated() {
            let matrix = record["related"] as! [[Int]]
            let related = matrix.map { row in Set(row.indices.filter { row[$0] == 1 }) }
            XCTAssertEqual(NativePhraseClassifier.reconcileNonHigh(roles: record["roles"] as! [Int], related: related),
                           record["expected"] as! [Int], "case \(i)")
        }
    }

    func testPowerAndCompressionMatchNativeExecution() throws {
        for record in try records("power") {
            let result = try NativePhraseClassifier.lowPower(low: record["low"] as! [Double], full: record["full"] as! [Double],
                                                           beatCount: record["beatCount"] as! Int)
            compare([result.ratio, result.maximumRatioMean, result.maximumPowerMean], record["expected"] as! [Double])
            compare(result.blockRatios, record["blockRatios"] as! [Double])
            compare(result.blockPowers, record["blockPowers"] as! [Double])
        }
        for record in try records("compression") {
            let result = try NativePhraseClassifier.compression(low: record["low"] as! [Double], full: record["full"] as! [Double],
                rhythm: (record["rhythm"] as! [Int]).map(UInt8.init), phase: record["phase"] as! Int)
            compare([result.first, result.second], record["expected"] as! [Double])
        }
    }

    func testRhythmHelpersMatchNativeExecution() throws {
        for record in try records("slowRock") {
            XCTAssertEqual(try NativePhraseClassifier.slowRock(low: (record["low"] as! [Int]).map(UInt8.init),
                middle: (record["middle"] as! [Int]).map(UInt8.init)), record["expected"] as! Int)
        }
        for record in try records("rhythmFlags") {
            let result = try NativePhraseClassifier.rhythmFlags(rhythm: (record["rhythm"] as! [Int]).map(UInt8.init),
                bpm: record["bpm"] as! Double, blockPowers: record["blockPowers"] as! [Double])
            XCTAssertEqual([result.fourKick, result.popRock, result.alternateRock, result.sparseKick], record["expected"] as! [Int])
        }
        for record in try records("slowTempo") {
            let result = try NativePhraseClassifier.slowTempo(sampleRate: record["sampleRate"] as! Double, bpm: record["bpm"] as! Double,
                blockPowers: record["blockPowers"] as! [Double], onsetSamples: record["onsetSamples"] as! [Int],
                beatSamples: record["beatSamples"] as! [Int], onsetCount: record["onsetCount"] as! Int, firstOnset: record["firstOnset"] as! Int,
                sourceOffsetMS: record["sourceOffsetMS"] as! Int, analysisOffsetMS: record["analysisOffsetMS"] as! Int)
            XCTAssertEqual([result.slow, result.syncopated], record["expected"] as! [Int])
        }
    }

    private func compare(_ actual: [Double], _ expected: [Double], file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(actual.count, expected.count, file: file, line: line)
        for (a, b) in zip(actual, expected) { XCTAssertEqual(a, b, accuracy: max(1e-11, abs(b) * 1e-11), file: file, line: line) }
    }
}
