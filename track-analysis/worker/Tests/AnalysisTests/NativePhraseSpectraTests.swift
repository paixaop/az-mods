import Foundation
import XCTest
@testable import Analysis

final class NativePhraseSpectraTests: XCTestCase {
    func testStageCBlockCutsAndTrailingPaddingMatchOriginalNativeExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseSpectralBlocks", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for record in root["spectralBlocks"] as! [[String: Any]] {
            let count = record["sourceFrameCount"] as! Int
            let middle = (0..<(count / 8)).map { i in sin(Double(i) * 0.017) * 0.7 + sin(Double(i) * 0.137) * 0.3 + 0.01 * Double(i % 7 - 3) }
            let samples = NativePhraseFeatures.Samples(sourceRate: record["sourceRate"] as! Double, sourceFrameCount: count,
                low: [Double](repeating: 0, count: count / 32), middle: middle, high: [Double](repeating: 0, count: count / 2))
            let timing = NativePhraseFeatures.Timing(bpm: record["bpm"] as! Double,
                offsetMilliseconds: record["offsetMilliseconds"] as! Int, phase: record["phase"] as! Int)
            let coordinates = try NativePhraseFeatures.coordinates(samples: samples, timing: timing)
            let blocks = try NativePhraseRhythmFeatures.blocks(samples: samples, timing: timing, coordinates: coordinates)
            XCTAssertEqual(blocks.map { [$0.firstBeat, $0.endBeat, $0.phase, $0.offsetMilliseconds, $0.middle.lowerBound, $0.middle.upperBound] }, record["blocks"] as! [[Int]])
            let actual = try NativePhraseSpectra.make(samples: samples, timing: timing, coordinates: coordinates, recurrence: record["recurrence"] as! Bool)
            let expected = record["expected"] as! [[Double]], rows = record["rowIndices"] as! [Int]
            XCTAssertEqual(actual.count, record["rowCount"] as! Int)
            var magnitudeError = 0.0, decibelError = 0.0
            for (row, reference) in zip(rows, expected) {
                XCTAssertEqual(actual[row].count, reference.count)
                for (a, b) in zip(actual[row], reference) {
                    magnitudeError = max(magnitudeError, abs(pow(10, a * 0.05) - pow(10, b * 0.05)))
                    decibelError = max(decibelError, abs(a - b))
                }
            }
            // Logarithms amplify transform roundoff near zero; compare the
            // original linear magnitude domain across the complete spectrum.
            print("Native spectral block comparison: max magnitude error \(magnitudeError), max dB error \(decibelError)")
            XCTAssertLessThan(magnitudeError, 1e-10)
        }
    }

    private func records(_ key: String) throws -> [[String: Any]] {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseSpectra", withExtension: "json"))
        let root = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(root["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        return try XCTUnwrap(root[key] as? [[String: Any]])
    }

    func testPeaksMatchOriginalNativeExecution() throws {
        for (i, record) in try records("peaks").enumerated() {
            let peaks = NativePhraseSpectra.peaks(record["input"] as! [Double])
            XCTAssertEqual(peaks.map(\.position), record["position"] as! [Int], "case \(i)")
            compare(peaks.map(\.height), record["height"] as! [Double])
            compare(peaks.map(\.prominence), record["prominence"] as! [Double])
            compare(peaks.map(\.width), record["width"] as! [Double])
        }
    }

    func testBothFFTWindowSizesAndBarFramingMatchOriginalNativeExecution() throws {
        for record in try records("fft") {
            let actual = try NativePhraseSpectra.make(samples: record["samples"] as! [Double],
                sampleRate: record["sampleRate"] as! Double, bpm: record["bpm"] as! Double,
                beatSamples: record["beatSamples"] as! [Int], phase: record["phase"] as! Int,
                endBeat: record["endBeat"] as! Int, recurrence: record["recurrence"] as! Bool)
            let expected = record["expected"] as! [[Double]]
            XCTAssertEqual(actual.count, expected.count)
            for (a, b) in zip(actual, expected) { compare(a, b, tolerance: 1e-7) }
        }
    }

    func testVocalDetectionsAndEntriesMatchOriginalNativeExecution() throws {
        for record in try records("vocal") {
            compare(try NativePhraseSpectra.vocalActivity(record["spectra"] as! [[Double]]),
                    record["expected"] as! [Double])
        }
        for (i, record) in try records("vocalEntries").enumerated() {
            XCTAssertEqual(NativePhraseSpectra.vocalEntries(record["input"] as! [Double]),
                           record["expected"] as! [Int], "case \(i)")
        }
    }

    private func compare(_ actual: [Double], _ expected: [Double], tolerance: Double = 1e-10,
                         file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(actual.count, expected.count, file: file, line: line)
        for (a, b) in zip(actual, expected) { XCTAssertEqual(a, b, accuracy: tolerance, file: file, line: line) }
    }
}
