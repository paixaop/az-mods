import Foundation
import XCTest
@testable import Analysis

final class NativePhraseTimingTests: XCTestCase {
    func testTimingRhythmAndCorrectionMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseTiming", withExtension: "json"))
        let fixture = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(fixture["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for (index, record) in (fixture["records"] as! [[String: Any]]).enumerated() {
            let mode = record["mode"] as! String, c = record["input"] as! [String: Any]
            let expected = record["expected"] as! [String: Any]
            let input = c["input"] as! [Double], rate = c["rate"] as! Double, bpm = c["bpm"] as! Double
            let offset = c["offset"] as! Int, phase = c["phase"] as! Int
            let coordinates = c["coordinates"] as! [Int], count = c["count"] as! Int
            let context = "\(mode) witness \(index)"
            func doubles(_ actual: [Double], _ key: String) {
                let reference = expected[key] as! [Double]
                XCTAssertEqual(actual.count, reference.count, context)
                XCTAssertLessThanOrEqual(zip(actual, reference).map { abs($0 - $1) }.max() ?? 0, 1e-10, "\(context) \(key)")
            }
            if mode == "timing-correction" {
                doubles(NativePhraseTiming.quarterPower(input, rate: rate, bpm: bpm, offset: offset,
                    cutoff: c["cutoff"] as! Double), "onset")
                doubles(NativePhraseTiming.quarterPower(input, rate: rate, bpm: bpm, offset: offset,
                    cutoff: 100, coordinates: coordinates), "bar")
                let snares = c["snares"] as! [Int], first = c["first"] as! Int, snareCount = c["snareCount"] as! Int
                let corrected = NativePhraseTiming.correctOffset(samples: input, rate: rate, bpm: bpm,
                    offset: offset, rhythm: (c["rhythm"] as! [Int]).map(UInt8.init), beatCount: count,
                    snares: snares, snareCount: snareCount, firstSnare: first)
                XCTAssertEqual([corrected.offset, corrected.type, corrected.rhythmType, corrected.strongAlignedBeat],
                    expected["offset"] as! [Int], context)
                let bar = NativePhraseTiming.correctBar(samples: input, rate: rate, bpm: bpm, originalOffset: offset,
                    correctedOffset: c["corrected"] as! Int, initialPhase: 1, coordinates: coordinates,
                    snares: snares, strengths: c["strengths"] as! [Double], count: snareCount, first: first,
                    suppliedPhase: phase, correctionType: c["type"] as! Int)
                doubles([Double(bar.phase), Double(bar.parity), 0, Double(bar.ones), Double(bar.valid), bar.ratio, 0, Double(bar.shifted)], "phase")
            } else {
                var attacks = NativePhraseRhythm.Attacks(rate: rate, second: c["second"] as! Bool,
                    delay: c["delay"] as! Int, previousEnvelope: c["previous"] as! Double,
                    maximumAttack: c["maximum"] as! Double)
                let pattern = mode == "rhythm"
                    ? attacks.process(input, rate: rate, bpm: bpm, offsetMilliseconds: offset, phase: phase,
                        block: c["block"] as! Int, coordinates: coordinates, beatCount: count)
                    : attacks.processTiming(input, rate: rate, bpm: bpm, offsetMilliseconds: offset,
                        phase: phase, block: c["block"] as! Int)
                XCTAssertEqual(pattern.map(Int.init), expected["pattern"] as! [Int], context)
                XCTAssertEqual(attacks.delay, expected["delay"] as! Int, context)
                XCTAssertEqual(attacks.previousEnvelope, expected["previous"] as! Double, accuracy: 1e-12, context)
                XCTAssertEqual(attacks.maximumAttack, expected["maximum"] as! Double, accuracy: 1e-12, context)
                if mode == "rhythm" {
                    var low = NativePhraseRhythm.PowerProcessor(rate: rate, low: true)
                    var all = NativePhraseRhythm.PowerProcessor(rate: rate, low: false)
                    let l = low.process(input, rate: rate, bpm: bpm, coordinates: coordinates, beatCount: count)
                    let a = all.process(input, rate: rate, bpm: bpm, coordinates: coordinates, beatCount: count)
                    doubles(l.quarters, "low"); doubles(l.bars, "bars")
                    doubles(a.quarters, "all"); doubles(a.peaks, "peaks")
                }
            }
        }
    }
}
