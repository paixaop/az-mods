import Foundation
import XCTest
@testable import Analysis

final class NativeBeatLegacyTests: XCTestCase {
    private struct Case: Decodable {
        let sampleRate: Int
        let sourceFrameCount: Int
        let pattern: Int
        let expected: [[Double]]
        let globalBPM: Float
        let detectedDynamic: Bool
        let extraBool: Bool
    }

    func testStreamingPrecursorMatchesCompleteOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatLegacy", withExtension: "json"))
        let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: url))
        for item in cases {
            let analyzer = NativeBeatLegacy(sampleRate: item.sampleRate, sourceFrameCount: item.sourceFrameCount)
            for first in stride(from: 0, to: item.sourceFrameCount + 1, by: 8192) {
                var samples: [Int16] = []
                for frame in first..<min(first + 8192, item.sourceFrameCount + 1) {
                    let sample = signal(frame: frame, sampleRate: item.sampleRate, pattern: item.pattern)
                    samples.append(sample)
                    samples.append(sample)
                }
                try analyzer.append(interleaved: samples, frameCount: samples.count / 2)
            }
            let result = try analyzer.finish()
            let actual = result.beats.map {
                [Double($0.sampleIndex), Double($0.intervalInBeats), Double($0.periodMilliseconds),
                 Double($0.beatInBar), Double($0.sourceBand), Double($0.flag)]
            }
            XCTAssertEqual(actual, item.expected, "\(item.sampleRate), pattern \(item.pattern)")
            XCTAssertEqual(result.globalBPM, item.globalBPM)
            XCTAssertEqual(result.detectedDynamic, item.detectedDynamic)
            XCTAssertEqual(result.extraBool, item.extraBool)
        }
    }

    private func signal(frame: Int, sampleRate: Int, pattern: Int) -> Int16 {
        func floorDivide(_ value: Int, by divisor: Int) -> Int {
            value / divisor - (value % divisor < 0 ? 1 : 0)
        }
        let period = pattern < 2 ? sampleRate / 2 : sampleRate * 3 / 5
        let position = frame % period
        var value = position < 3500
            ? floorDivide(((frame * 97) % 32768 - 16384) * (3500 - position), by: 3500) : 0
        if pattern == 1 {
            let hiHat = frame % (period / 4)
            if hiHat < 400 {
                value += floorDivide(((frame * 991) % 8192 - 4096) * (400 - hiHat), by: 400)
            }
        }
        if pattern == 2 && frame / period % 7 == 3 { value = 0 }
        return Int16(value)
    }
}
