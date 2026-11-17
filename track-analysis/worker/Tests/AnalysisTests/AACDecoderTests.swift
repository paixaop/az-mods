import Foundation
import XCTest
import PortableDSP
@testable import Analysis

final class AACDecoderTests: XCTestCase {
    struct Provenance: Decodable {
        struct Witness: Decodable {
            let file: String
            let sampleRate, channels, packetCount, sourceDelayFrames: Int
            let editStart, presentationFrames: Int?
            let pulseFrames, referencePeakFrames: [Int]
        }
        let cases: [Witness]
    }
    func witnesses() throws -> (URL, [Provenance.Witness]) {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "AAC-Provenance", withExtension: "json"))
        return (url.deletingLastPathComponent(), try JSONDecoder().decode(Provenance.self, from: Data(contentsOf: url)).cases)
    }
    func assertFrame(_ actual: Int, _ expected: Int, rate: Int, window: UInt64 = 1,
                     file: StaticString = #filePath, line: UInt = #line) {
        let order = compare_timestamps(Double(actual), Double(expected), UInt64(rate), Double(rate), window)
        XCTAssertNotEqual(order, AZ_TIME_INVALID, file: file, line: line)
        XCTAssertEqual(order, AZ_TIME_EQUAL, file: file, line: line)
    }
    func testAACEncodedClockImpulsePositionsAndExactSeeks() throws {
        let (root, cases) = try witnesses()
        for witness in cases {
            let decoder = try PCMDecoder(url: root.appendingPathComponent(witness.file))
            XCTAssertEqual(decoder.rate, witness.sampleRate, witness.file)
            XCTAssertEqual(decoder.clock.kind, UInt32(TA_DECODE_AAC_ENCODED))
            assertFrame(decoder.frameCount, witness.packetCount * 1024, rate: decoder.rate)
            assertFrame(Int(decoder.clock.decoder_leading_frames), 1024, rate: decoder.rate)
            XCTAssertEqual(decoder.clock.has_edit, witness.editStart == nil ? 0 : 1)
            if let start = witness.editStart, let count = witness.presentationFrames {
                assertFrame(Int(decoder.clock.edit_start_frames), start, rate: decoder.rate)
                assertFrame(Int(decoder.clock.presentation_frames), count, rate: decoder.rate)
            }
            var samples = [Int16]()
            while let block = try decoder.read(frames: 997) { samples += block }
            assertFrame(samples.count / 2, decoder.frameCount, rate: decoder.rate)
            XCTAssertTrue(samples.prefix(2048).allSatisfy { $0 == 0 })
            for (pulse, referencePeak) in zip(witness.pulseFrames, witness.referencePeakFrames) {
                let expected = pulse + witness.sourceDelayFrames
                let search = (expected - 64)..<(expected + 64)
                let peak = try XCTUnwrap(search.max { abs(Int(samples[$0 * 2])) < abs(Int(samples[$1 * 2])) })
                assertFrame(peak, referencePeak, rate: decoder.rate, window: 2)
                assertFrame(peak, expected + 7, rate: decoder.rate, window: 4)
                XCTAssertGreaterThan(abs(Int(samples[peak * 2])), 10000, witness.file)
            }
            for range in [1021..<2053, 33000..<34003, 511..<1535, 0..<2048] {
                let replay = try decoder.mono(range)
                for index in replay.indices {
                    let at = (range.lowerBound + index) * 2
                    let reference = (Float(samples[at]) + Float(samples[at + 1])) / 65536
                    XCTAssertEqual(replay[index], reference, witness.file)
                }
            }
        }
    }
    func temporary(_ data: Data, _ body: (URL) throws -> Void) throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try data.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        try body(url)
    }
    func testTruncatedAndChangingADTSFormatsAreRejected() throws {
        let (root, _) = try witnesses()
        let data = try Data(contentsOf: root.appendingPathComponent("lc-44100-2.aac"))
        try temporary(data.dropLast()) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
        var changed = data
        let next = (Int(data[3]) & 3) << 11 | Int(data[4]) << 3 | Int(data[5]) >> 5
        changed[next + 2] = (changed[next + 2] & 0xc3) | (3 << 2)
        try temporary(changed) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
        changed = data
        changed[2] = (changed[2] & 0x3f) | 0x80 // SSR is outside AAC-LC.
        try temporary(changed) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
    }
    func testMP4BoundsAndUnsupportedEditsAreRejected() throws {
        let (root, _) = try witnesses()
        let data = try Data(contentsOf: root.appendingPathComponent("lc-44100-2.m4a"))
        try temporary(data.dropLast()) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
        var changed = data
        let edit = try XCTUnwrap(data.range(of: Data("elst".utf8))).lowerBound
        changed[edit + 11] = 2 // Two edit entries cannot fit this single-entry box.
        try temporary(changed) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
        changed = data
        let stsz = try XCTUnwrap(data.range(of: Data("stsz".utf8))).lowerBound
        changed.replaceSubrange((stsz + 12)..<(stsz + 16), with: [255, 255, 255, 255])
        try temporary(changed) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
        changed = data
        let format = try XCTUnwrap(data.range(of: Data("mp4a".utf8))).lowerBound
        changed.replaceSubrange(format..<(format + 4), with: Data("enca".utf8))
        try temporary(changed) { XCTAssertThrowsError(try PCMDecoder(url: $0)) }
    }
    func testTwoActiveAACDecodersHaveIndependentState() throws {
        let (root, cases) = try witnesses()
        let first = try PCMDecoder(url: root.appendingPathComponent(cases[0].file))
        let second = try PCMDecoder(url: root.appendingPathComponent(cases[1].file))
        XCTAssertEqual(try first.mono(10000..<14000), try second.mono(10000..<14000))
        _ = try first.mono(30000..<34000)
        XCTAssertEqual(try first.mono(10000..<14000), try second.mono(10000..<14000))
    }
    func testHEAACIsExplicitlyRejected() throws {
        let (root, _) = try witnesses()
        XCTAssertThrowsError(try PCMDecoder(url: root.appendingPathComponent("unsupported-he-aac.m4a")))
    }
}
