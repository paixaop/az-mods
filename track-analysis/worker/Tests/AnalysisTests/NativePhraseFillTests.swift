import Foundation
import XCTest
@testable import Analysis

final class NativePhraseFillTests: XCTestCase {
    func testSyntheticExecutionWitnessesCoverFiltersFillsAndConnectedHighDecorators() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativePhraseFills", withExtension: "json"))
        let fixture = try XCTUnwrap(fixtureJSONObject(with: Data(contentsOf: url)) as? [String: Any])
        XCTAssertEqual(fixture["sourceSHA256"] as? String, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        let cases = try XCTUnwrap(fixture["records"] as? [[String: Any]])
        var failures = [[String: Any]]()
        var maximumEnvelopeError = 0.0
        for (index, row) in cases.enumerated() {
            let mode = row["mode"] as! String
            if mode == "stageC" {
                let values = row["coords"] as! [[Int]]
                let samples = NativePhraseFeatures.Samples(sourceRate: row["rate"] as! Double, sourceFrameCount: row["sourceFrameCount"] as! Int,
                    low: row["low"] as! [Double], middle: row["middle"] as! [Double], high: row["high"] as! [Double])
                let coordinates = NativePhraseFeatures.Coordinates(beatCount: row["count"] as! Int, high: values[2], middle: values[1], low: values[0])
                let timing = NativePhraseFeatures.Timing(bpm: row["bpm"] as! Double, offsetMilliseconds: row["offset"] as! Int, phase: row["phase"] as! Int)
                let actual = try NativePhraseRhythmFeatures.make(samples: samples, timing: timing, coordinates: coordinates)
                let geometry = try NativePhraseRhythmFeatures.blocks(samples: samples, timing: timing, coordinates: coordinates).map {
                    [$0.firstBeat,$0.endBeat,$0.phase,$0.offsetMilliseconds,$0.low.lowerBound,$0.middle.lowerBound,$0.high.lowerBound,
                     $0.low.upperBound,$0.middle.upperBound,$0.high.upperBound]
                }
                if geometry != row["geometry"] as! [[Int]] { failures.append(["index":index,"actualGeometry":geometry]) }
                let expected = row["expected"] as! [String:Any]
                let floating = ["lowPower":actual.lowPower,"subPower":actual.subPower,"allPower":actual.allPower,"allPeaks":actual.allPeaks,
                    "dividedLow":actual.dividedLow,"dividedMiddle":actual.dividedMiddle]
                for (key, value) in floating {
                    let reference = expected[key] as! [Double]
                    let error = zip(value, reference).map { abs($0 - $1) }.max() ?? 0
                    if value.count != reference.count || error > 1e-9 { failures.append(["index":index,"key":key,"error":error,"actual":value]) }
                }
                let integers = ["firstAttacks":actual.firstAttacks.map(Int.init),"secondAttacks":actual.secondAttacks.map(Int.init),
                    "hiHatCounts":actual.hiHatCounts.map(Int.init),"hiHatChanges":actual.hiHatChanges.map(Int.init),"finalMiddleCoordinates":actual.finalMiddleCoordinates]
                for (key,value) in integers where value != expected[key] as! [Int] { failures.append(["index":index,"key":key,"actual":value]) }
            } else if mode == "hiHatChanges" {
                let actual = NativePhraseRhythmFeatures.hiHatChanges((row["values"] as! [Int]).map(UInt8.init), phase: row["phase"] as! Int).map(Int.init)
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual]) }
            } else if mode == "hiHatCounts" {
                let actual = NativePhraseRhythmFeatures.hiHatCounts(samples: row["samples"] as! [Double], sampleRate: row["rate"] as! Double,
                    bpm: row["bpm"] as! Double, coordinates: row["coords"] as! [Int], beatCount: row["count"] as! Int,
                    fast: row["fast"] as! Double, slow: row["slow"] as! Double).map(Int.init)
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual]) }
            } else if mode == "subPower" {
                let rate = row["rate"] as! Double
                var sub = NativePhraseRhythmFeatures.SubPowerProcessor(rate: rate)
                for (block, item) in (row["blocks"] as! [[String:Any]]).enumerated() {
                    let actual = sub.process(item["samples"] as! [Double], rate: rate, bpm: row["bpm"] as! Double,
                        coordinates: item["coords"] as! [Int], beatCount: row["count"] as! Int)
                    let error = zip(actual,item["expected"] as! [Double]).map { abs($0 - $1) }.max() ?? 0
                    if error > 1e-10 { failures.append(["index": index, "block": block, "error": error]) }
                }
            } else if mode == "snareTimingFilter" {
                let actual = NativePhraseSnareTiming.filter(row["samples"] as! [Double], sampleRate: row["rate"] as! Double)
                let error = zip(actual, row["expected"] as! [Double]).map { abs($0 - $1) }.max() ?? 0
                if error > 1e-12 { failures.append(["index": index, "error": error]) }
            } else if mode == "snareTiming" {
                let rate = row["rate"] as! Double, bpm = row["bpm"] as! Double, offset = row["offset"] as! Int, count = row["count"] as! Int
                let base = NativePhraseSnareTiming.detect(filtered: row["filtered"] as! [Double], sampleRate: rate, bpm: bpm, offsetMilliseconds: offset,
                    beatCoordinates: row["coords"] as! [Int], count: count)
                let rhythm = (row["rhythm"] as! [Int]).map(UInt8.init)
                let genre = NativePhraseSnareTiming.forGenre(windows: base.windows, sampleRate: rate, bpm: bpm, offsetMilliseconds: offset,
                    count: count, rhythm: rhythm, fourKick: row["fourKick"] as! Int)
                let bar = NativePhraseSnareTiming.forBar(windows: base.windows, sampleRate: rate, bpm: bpm, offsetMilliseconds: offset, count: count, rhythm: rhythm)
                let expected = row["expected"] as! [[String:Any]]
                for (mode, actual) in [base.positions,genre,bar].enumerated() {
                    let e = expected[mode]
                    let error = zip(actual.strengths, e["strengths"] as! [Double]).map { abs($0 - $1) }.max() ?? 0
                    if actual.values != e["positions"] as! [Int] || actual.count != e["count"] as! Int || actual.first != e["first"] as! Int || error > 1e-12 {
                        failures.append(["index": index, "submode": mode, "actual": actual.values, "actualStrengths": actual.strengths, "count": actual.count, "first": actual.first])
                    }
                }
            } else if mode == "openingFeatures" {
                let actual = NativePhraseOpening.spectralFeatures(spectra: row["spectra"] as! [[Double]], frameCount: row["frameCount"] as! Int,
                    firstFrame: row["firstFrame"] as! Int, beatCount: row["beatCount"] as! Int)
                let expected = row["expected"] as! [[Double]]
                let error = zip(actual.envelope, expected[2]).map { abs($0 - $1) }.max() ?? 0
                if actual.presence.map(Double.init) != expected[0] || actual.rises.map(Double.init) != expected[1] || error > 1e-12 {
                    failures.append(["index": index, "actual": [actual.presence.map(Double.init),actual.rises.map(Double.init),actual.envelope], "error": error])
                }
            } else if mode == "openingSpectra" {
                let actual = try NativePhraseOpening.makeSpectra(samples: row["samples"] as! [Double], sampleRate: row["rate"] as! Double)
                let expected = row["expected"] as! [[Double]]
                let error = zip(actual, expected).flatMap { zip($0, $1).map { abs($0 - $1) } }.max() ?? 0
                let first = NativePhraseOpening.firstSpectralFrame(sampleRate: row["rate"] as! Double, bpm: row["bpm"] as! Double, firstBeat: row["beat"] as! Int, offsetMilliseconds: row["offset"] as! Int)
                if error > 1e-8 || first != row["expectedFirst"] as! Int { failures.append(["index": index, "error": error, "first": first, "actual": actual]) }
            } else if mode == "snare" {
                let actual = NativePhraseFills.snareCandidates(normalizedHigh: row["values"] as! [Double], bars: row["bars"] as! Int)
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual]) }
            } else if mode == "high" {
                let profile = NativePhraseFills.Profile(low: row["low"] as! [Double], lowQuarterMean: row["fine"] as! [Double],
                    highEighths: row["high"] as! [Double], powerQuarters: row["power"] as! [Double], phase: row["phase"] as! Int, lowQuarters: row["quarters"] as! [Double])
                let actual = NativePhraseDecorators.high(boundaries: row["boundaries"] as! [Int], roles: row["roles"] as! [Int], profile: profile,
                    firstLevel: row["first"] as! [Double], secondLevel: row["second"] as! [Double], phraseMetric: row["metric"] as! [Double], activity: (row["activity"] as! [Int]).map(UInt8.init))
                let result = [actual.fills.roles, actual.fills.starts, actual.fills.auxiliary, actual.upStarts, actual.upFirst, actual.upSecond, actual.upThird]
                if result != row["expected"] as! [[Int]] || actual.boundaries.last != row["expectedTerminal"] as? Int {
                    failures.append(["index": index, "actual": result, "actualTerminal": actual.boundaries.last!])
                }
            } else if mode == "outro" {
                let result = NativePhraseDecorators.outro(boundaries: row["boundaries"] as! [Int], roles: row["roles"] as! [Int], low: row["low"] as! [Double],
                    activity: (row["activity"] as! [Int]).map(UInt8.init), quarters: row["quarters"] as! [Double], phase: row["phase"] as! Int)
                if result.roles != row["expectedRoles"] as! [Int] || result.terminal != row["expectedTerminal"] as! Int {
                    failures.append(["index": index, "actualRoles": result.roles, "actualTerminal": result.terminal])
                }
            } else if mode == "divided" {
                let positions = row["positions"] as! [Int]
                let samples = NativePhraseFeatures.Samples(sourceRate: row["rate"] as! Double, sourceFrameCount: 0, low: row["low"] as! [Double], middle: row["middle"] as! [Double], high: [])
                let coordinates = NativePhraseFeatures.Coordinates(beatCount: positions.count, high: positions, middle: positions, low: positions)
                let result = NativePhraseDecorators.dividedBands(samples: samples, coordinates: coordinates)
                let error = zip([result.low, result.middle], row["expected"] as! [[Double]]).flatMap { zip($0, $1).map { abs($0 - $1) } }.max() ?? 0
                if error > 1e-10 { failures.append(["index": index, "maxError": error]) }
            } else if mode == "beatFour" {
                let actual = NativePhraseDecorators.beatFour(start: row["start"] as! Int, end: row["end"] as! Int,
                    lastBoundary: row["last"] as! Bool, low: row["low"] as! [Double], quarters: row["quarters"] as! [Double])
                if actual != row["expected"] as! Bool { failures.append(["index": index, "actual": actual]) }
            } else if mode == "loopRestore" {
                let actual = NativePhraseDecorators.restoreLoops(roles: row["roles"] as! [Int], saved: row["saved"] as! [Int], metric: row["metric"] as! [Double])
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual]) }
            } else if mode == "simpleLoop" {
                let actual = NativePhraseDecorators.simpleLoops(boundaries: row["boundaries"] as! [Int], roles: row["roles"] as! [Int],
                    firstLevel: row["first"] as! [Double], secondLevel: row["second"] as! [Double], phraseMetric: row["metric"] as! [Double])
                let error = zip(actual.metrics, row["expectedMetric"] as! [Double]).map { abs($0 - $1) }.max() ?? 0
                if actual.roles != row["expectedRoles"] as! [Int] || actual.starts != row["expectedStarts"] as! [Int] || error > 1e-10 {
                    failures.append(["index": index, "actualRoles": actual.roles, "actualStarts": actual.starts, "actualMetric": actual.metrics])
                }
            } else if mode == "rhythm" {
                let actual = NativePhraseDecorators.rhythm(boundaries: row["boundaries"] as! [Int], roles: row["roles"] as! [Int], activity: (row["activity"] as! [Int]).map(UInt8.init))
                let outputs = [actual.roles, actual.starts, actual.first, actual.second, actual.third]
                if outputs != row["expected"] as! [[Int]] { failures.append(["index": index, "actual": outputs]) }
            } else if mode == "cut" || mode == "growth" {
                let low = row["low"] as! [Double], end = row["end"] as! Int
                let detection = mode == "cut"
                    ? NativePhraseFills.cut(start: 1, end: end, low: low, lowQuarterMean: row["fine"] as! [Double])
                    : NativePhraseFills.growth(start: 1, end: end, low: low)
                let actual = [detection?.start ?? 0, detection?.auxiliaryEnd ?? 0]
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual]) }
            } else if mode == "final" {
                let roles = row["roles"] as! [Int], starts = row["starts"] as! [Int]
                var result = NativePhraseFills.Result(roles: roles, starts: starts,
                    auxiliary: Array(repeating: 0, count: roles.count), routes: Array(repeating: nil, count: roles.count))
                NativePhraseFills.finalAdjust(boundaries: row["boundaries"] as! [Int], highMood: row["high"] as! Bool,
                    phraseMetric: row["metric"] as! [Double], result: &result)
                if result.roles != row["expectedRoles"] as! [Int] || result.starts != row["expectedStarts"] as! [Int] {
                    failures.append(["index": index, "actualRoles": result.roles, "actualStarts": result.starts])
                }
            } else if mode == "upper" {
                let high = NativePhraseFills.HighProfile(values: row["values"] as! [Double], supportedBeats: row["supported"] as! Int)
                let scores = NativePhraseFills.repeatedScores(high)
                let detection = NativePhraseFills.repeatedUpper(end: row["end"] as! Int, origin: 1, scores: scores)
                let actual = [detection?.start ?? 0, detection?.auxiliaryEnd ?? 0]
                if actual != row["expected"] as! [Int] { failures.append(["index": index, "actual": actual, "scores": scores]) }
            } else if mode == "highNormalize" {
                let output = NativePhraseFills.normalizeHigh(row["values"] as! [Double], phase: row["phase"] as! Int)!
                let error = zip(output.values, row["expected"] as! [Double]).map { abs($0 - $1) }.max() ?? 0
                if error > 1e-14 || output.supportedBeats != row["supported"] as! Int {
                    failures.append(["index": index, "maxError": error])
                }
            } else if mode == "cleanup" {
                let roles = row["roles"] as! [Int]
                var result = NativePhraseFills.Result(roles: roles, starts: row["starts"] as! [Int],
                    auxiliary: Array(repeating: 0, count: roles.count), routes: Array(repeating: nil, count: roles.count))
                let profile = NativePhraseFills.Profile(low: row["low"] as! [Double], lowQuarterMean: [], highEighths: [],
                    powerQuarters: row["power"] as! [Double], phase: 1)
                NativePhraseFills.cleanActivity(boundaries: row["boundaries"] as! [Int], profile: profile, result: &result)
                if result.roles != row["expectedRoles"] as! [Int] || result.starts != row["expectedStarts"] as! [Int]
                    || result.auxiliary != row["expectedAuxiliary"] as! [Int] {
                    failures.append(["index": index, "actualRoles": result.roles, "actualStarts": result.starts, "actualAuxiliary": result.auxiliary])
                }
            } else if mode == "envelope" {
                let samples = row["values"] as! [Double]
                let streams = try NativePhraseEnvelopes.make(lowInput: samples, middleInput: samples, highInput: samples, sourceRate: row["sourceRate"] as! Double)
                let actual = [streams.low, streams.middle, streams.high, streams.lowLevel, streams.middleLevel]
                let expected = row["expected"] as! [[Double]]
                let error = zip(actual, expected).flatMap { zip($0, $1).map { abs($0 - $1) } }.max() ?? 0
                maximumEnvelopeError = max(maximumEnvelopeError, error)
                if error > 1e-12 { failures.append(["index": index, "maxError": error]) }
            }
        }
        XCTAssertTrue(failures.isEmpty, "Native execution mismatches: \(failures)")
        XCTAssertLessThan(maximumEnvelopeError, 1e-12)
    }

    func testFilterStateContinuesAcrossDecodedChunks() {
        let input = (0..<129).map { sin(Double($0) * 0.21) }
        var whole = NativePhraseEnvelopes.secondOrder(rate: 5512.5, angularCutoff: 3141.592653589793,
            damping: 1.4142135623730951, highPass: true)
        var chunked = whole
        let expected = whole.process(input)
        let actual = chunked.process(Array(input[..<37])) + chunked.process(Array(input[37..<89]))
            + chunked.process(Array(input[89...]))
        XCTAssertEqual(actual, expected)
    }

    func testZeroDataFlagUsesExactLowBandNonzeroCheck() throws {
        let timing = NativePhraseFeatures.Timing(bpm: 120, offsetMilliseconds: 0, phase: 1)
        func analyze(_ first: Double) throws -> Int {
            let samples = NativePhraseFeatures.Samples(sourceRate: 32000, sourceFrameCount: 32000,
                low: [first] + Array(repeating: 0, count: 999),
                middle: Array(repeating: 0, count: 4000), high: Array(repeating: 0, count: 16000))
            let coordinates = try NativePhraseFeatures.coordinates(samples: samples, timing: timing)
            return try NativePhraseRhythmFeatures.make(samples: samples, timing: timing, coordinates: coordinates).zeroDataFlag
        }
        XCTAssertEqual(try analyze(-0.0), 0)
        XCTAssertEqual(try analyze(1e-300), 1)
    }
}
