import Foundation
import XCTest
@testable import Analysis

final class PortableDecoderTests: XCTestCase {
    struct Provenance: Decodable {
        let cases: [Witness]
        struct Witness: Decodable {
            let file: String
            let sampleRate: Int
            let sampleCount: Int
            let prefix: String
        }
    }
    func testEncodedMPEGClockAndNativePrefixes() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "PROVENANCE", withExtension: "json"))
        let root = url.deletingLastPathComponent()
        let fixture = try JSONDecoder().decode(Provenance.self, from: Data(contentsOf: url))
        for witness in fixture.cases {
            let decoder = try PCMDecoder(url: root.appendingPathComponent(witness.file))
            XCTAssertEqual(decoder.rate, witness.sampleRate)
            let countOrder = compare_timestamps(Double(decoder.frameCount), Double(witness.sampleCount),
                UInt64(decoder.rate), Double(decoder.rate), 1)
            XCTAssertNotEqual(countOrder, AZ_TIME_INVALID)
            XCTAssertEqual(countOrder, AZ_TIME_EQUAL, witness.file)
            let expected = try Data(contentsOf: root.appendingPathComponent(witness.prefix))
                .withUnsafeBytes { Array($0.bindMemory(to: Int16.self)) }
            let first = try XCTUnwrap(decoder.read())
            let error = zip(first, expected).map { abs(Int($0) - Int($1)) }.max() ?? 0
            XCTAssertLessThanOrEqual(error, 1, witness.file)
            var frames = first.count / 2
            while let block = try decoder.read() { frames += block.count / 2 }
            let readOrder = compare_timestamps(Double(frames), Double(witness.sampleCount),
                UInt64(decoder.rate), Double(decoder.rate), 1)
            XCTAssertNotEqual(readOrder, AZ_TIME_INVALID)
            XCTAssertEqual(readOrder, AZ_TIME_EQUAL, witness.file)
            let replay = try decoder.mono(0..<1024)
            for i in replay.indices {
                let reference = (Float(first[i * 2]) + Float(first[i * 2 + 1])) * (1 / 65536)
                XCTAssertEqual(replay[i], reference, witness.file)
            }
            let shifted = try decoder.mono(512..<1536)
            for i in shifted.indices {
                let at = i + 512
                let reference = (Float(first[at * 2]) + Float(first[at * 2 + 1])) * (1 / 65536)
                XCTAssertEqual(shifted[i], reference, witness.file)
            }
        }
    }
    func testMixedMPEGFormatIsRejected() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "mixed-format", withExtension: "mp3"))
        XCTAssertThrowsError(try PCMDecoder(url: url))
    }
}
