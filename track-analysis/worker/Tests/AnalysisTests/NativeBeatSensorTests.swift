import Foundation
import XCTest
@testable import Analysis

final class NativeBeatSensorTests: XCTestCase {
    private struct SensorCase: Decodable {
        let sampleRate: Int
        let sourceFrameCount: Int
        let pattern: Int
        let expected: [[Double]]
    }
    private struct WindowCase: Decodable {
        let lowIntegrals: [Double]
        let globalBPM: Float
        let sampleRate: Int
        let expected: Int
    }
    private struct PeriodCase: Decodable {
        let interval: Float
        let previous: [Float]
        let expected: Expected?
        struct Expected: Decodable {
            let periodMilliseconds: Float
            let error: Float
            let matchedIntervals: Int
        }
    }

    private struct FilterCase: Decodable {
        let name: String
        let sampleRate: Float
        let input: [Float]
        let expected: [Float]
        let splits: [Int]
    }

    func testStreamingFiltersMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatFilters", withExtension: "json"))
        let cases = try JSONDecoder().decode([FilterCase].self, from: Data(contentsOf: url))
        for item in cases {
            var filter: NativeBeatFilters
            switch item.name {
            case "low": filter = .low(cutoff: 283, sampleRate: item.sampleRate, damping: 0.70710677)
            case "high": filter = .high(cutoff: 7900, sampleRate: item.sampleRate, damping: 0.70710677)
            case "middle": filter = .middle(lower: 283, upper: 7900, sampleRate: item.sampleRate, damping: 0.70710677)
            case "band": filter = .band(center: 150, sampleRate: item.sampleRate, width: 1.5)
            default: filter = .through()
            }
            var actual = [Float]()
            var start = 0
            for count in item.splits {
                actual += filter.process(Array(item.input[start..<(start + count)]))
                start += count
            }
            XCTAssertEqual(actual, item.expected, "\(item.name) at \(item.sampleRate)")
        }
    }

    func testOnsetPeriodCandidatesMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatPeriods", withExtension: "json"))
        let cases = try JSONDecoder().decode([PeriodCase].self, from: Data(contentsOf: url))
        for item in cases {
            let actual = NativeBeatPeriodCandidates.calculate(interval: item.interval, previous: item.previous)
            XCTAssertEqual(actual?.periodMilliseconds, item.expected?.periodMilliseconds)
            XCTAssertEqual(actual?.error, item.expected?.error)
            XCTAssertEqual(actual?.matchedIntervals, item.expected?.matchedIntervals)
        }
    }

    func testModelStartWindowMatchesOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatModelWindow", withExtension: "json"))
        let cases = try JSONDecoder().decode([WindowCase].self, from: Data(contentsOf: url))
        for item in cases {
            XCTAssertEqual(NativeBeatModelWindow.offset(lowIntegrals: item.lowIntegrals,
                globalBPM: item.globalBPM, sampleRate: item.sampleRate), item.expected)
        }
    }

    func testCompleteOnsetCandidatesMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatSensor", withExtension: "json"))
        let cases = try JSONDecoder().decode([SensorCase].self, from: Data(contentsOf: url))
        func floorDivide(_ value: Int, by divisor: Int) -> Int {
            value / divisor - (value % divisor < 0 ? 1 : 0)
        }
        for item in cases {
            let sensor = NativeBeatSensor(sampleRate: item.sampleRate, sourceFrameCount: item.sourceFrameCount)
            let blockSize = item.sampleRate / 75
            for first in stride(from: 0, to: item.sourceFrameCount, by: blockSize) {
                let samples = (first..<min(first + blockSize, item.sourceFrameCount)).map { i -> Float in
                    let period = item.pattern < 2 ? item.sampleRate / 2 : item.sampleRate * 3 / 5
                    let position = i % period
                    var value = position < 3500 ? floorDivide(((i * 97) % 32768 - 16384) * (3500 - position), by: 3500) : 0
                    if item.pattern == 2 && i / period % 7 == 3 { value = 0 }
                    return Float(value) / 32768
                }
                try sensor.append(normalizedMono: samples)
            }
            let actual = sensor.candidates.map {
                [Double($0.sampleIndex), Double($0.intervalInBeats), Double($0.periodMilliseconds),
                 Double($0.beatInBar), Double($0.sourceBand), Double($0.flag)]
            }
            XCTAssertEqual(actual, item.expected, "\(item.sampleRate), pattern \(item.pattern)")
        }
    }
}
