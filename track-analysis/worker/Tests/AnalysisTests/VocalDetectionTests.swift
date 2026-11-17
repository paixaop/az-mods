import Foundation
import XCTest
@testable import Analysis

final class VocalDetectionTests: XCTestCase {
    private func wav(rate: Int, frames: Int) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".wav")
        var bytes = Data()
        func ascii(_ s: String) { bytes.append(contentsOf: s.utf8) }
        func u16(_ n: Int) { bytes.append(UInt8(truncatingIfNeeded: n)); bytes.append(UInt8(truncatingIfNeeded: n >> 8)) }
        func u32(_ n: Int) { u16(n); u16(n >> 16) }
        ascii("RIFF"); u32(36 + frames * 4); ascii("WAVEfmt "); u32(16); u16(1); u16(2)
        u32(rate); u32(rate * 4); u16(4); u16(16); ascii("data"); u32(frames * 4)
        bytes.append(Data(repeating: 0, count: frames * 4))
        try bytes.write(to: url)
        return url
    }

    func testVocalOnlyWithoutGridTempoOrModelsAtBothSourceRates() throws {
        for rate in [44100, 48000] {
            let frames = rate + 17
            let url = try wav(rate: rate, frames: frames)
            defer { try? FileManager.default.removeItem(at: url) }
            // A bogus grid path must never be opened for vocal-only work.
            let result = try TrackAnalysisWorker.analyze(audio: url,
                savedDAT: URL(fileURLWithPath: "/nonexistent-vocal-test.DAT"), globalBPM: 0, needed: 32)
            XCTAssertEqual(String(data: result.prefix(4), encoding: .utf8), "TA02")
            XCTAssertEqual(result.be32(4), 0); XCTAssertEqual(result.be32(8), 0)
            XCTAssertEqual(result.be32(16), rate)
            XCTAssertEqual(compare_timestamps(Double(result.be32(20)), Double(frames), UInt64(rate), Double(rate), 1), AZ_TIME_EQUAL)
            let tag = Data(result.dropFirst(24))
            XCTAssertEqual(String(data: tag.prefix(4), encoding: .utf8), "PVDI")
            XCTAssertEqual(tag.be32(4), 24); XCTAssertEqual(tag.be32(8), tag.count)
            XCTAssertEqual(tag.be32(12), 1024); XCTAssertEqual(tag.be16(16), 22050); XCTAssertEqual(tag.be16(18), 1)
            XCTAssertEqual(tag.be32(20), 22)
            XCTAssertEqual(result.be32(12), tag.count)
            XCTAssertTrue(tag.dropFirst(24).allSatisfy { $0 == 0 })
        }
    }

    func testInvalidRequestsDoNotDecode() {
        for needed: UInt32 in [0, 4, 16, 32 | 64] {
            XCTAssertThrowsError(try TrackAnalysisWorker.analyze(audio: URL(fileURLWithPath: "/missing.wav"),
                savedDAT: nil, globalBPM: 0, needed: needed)) { error in
                guard case WorkerError.outOfRange = error else { return XCTFail("Unexpected error: \(error)") }
            }
        }
    }

    func testUniformTimelineIncludesShortTracksAndPartialFinalFrame() throws {
        for rate in [44100, 48000] {
            for frames in [100, 2048, rate * 2 + 137] {
                let samples = NativePhraseFeatures.Samples(sourceRate: Double(rate), sourceFrameCount: frames,
                    low: [], middle: Array(repeating: 0, count: frames / 8), high: [])
                let tag = try VocalDetection.encode(samples: samples, sourceFrames: frames)
                let count = tag.be32(20)
                let expected = (frames * 22050 + rate * 1024 - 1) / (rate * 1024)
                XCTAssertEqual(count, expected); XCTAssertEqual(tag.count, 24 + count)
                XCTAssertTrue(tag.dropFirst(24).allSatisfy { $0 == 0 })
            }
        }
    }
}
