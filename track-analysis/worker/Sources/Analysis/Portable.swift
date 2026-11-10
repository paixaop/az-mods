/* SPDX-License-Identifier: GPL-3.0-only */
#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP
import PortableDSP

let CblasRowMajor: Int32 = 101, CblasNoTrans: Int32 = 111, CblasTrans: Int32 = 112
func cblas_sgemv(_ order: Int32, _ transpose: Int32, _ rows: Int32, _ columns: Int32,
                 _ alpha: Float, _ matrix: UnsafePointer<Float>, _ stride: Int32,
                 _ input: UnsafePointer<Float>, _ inputStride: Int32, _ beta: Float,
                 _ output: UnsafeMutablePointer<Float>, _ outputStride: Int32) {
    precondition(order == CblasRowMajor && alpha == 1 && beta == 0 && inputStride == 1 && outputStride == 1)
    ta_gemv(transpose == CblasTrans ? 1 : 0, rows, columns, matrix, stride, input, output)
}
func cblas_sgemm(_ order: Int32, _ transposeA: Int32, _ transposeB: Int32,
                 _ rows: Int32, _ columns: Int32, _ inner: Int32, _ alpha: Float,
                 _ a: UnsafePointer<Float>, _ aStride: Int32, _ b: UnsafePointer<Float>,
                 _ bStride: Int32, _ beta: Float, _ output: UnsafeMutablePointer<Float>, _ outputStride: Int32) {
    precondition(order == CblasRowMajor && transposeA == CblasNoTrans && alpha == 1 && beta == 0)
    ta_gemm(transposeB == CblasTrans ? 1 : 0, rows, columns, inner, a, aStride, b, bStride, output, outputStride)
}

final class PCMDecoder {
    let handle: OpaquePointer
    let rate: Int
    let frameCount: Int
    init(url: URL) throws {
        var rate: UInt32 = 0, frames: UInt64 = 0
        guard let handle = ta_decode_open(url.path, &rate, &frames), frames > 1 else {
            throw AnalysisError.decodingFailed("Unsupported, invalid or over-20-minute audio")
        }
        self.handle = handle; self.rate = Int(rate); frameCount = Int(frames)
    }
    deinit { ta_decode_close(handle) }
    func read(frames: Int = 16384) throws -> [Int16]? {
        try Task.checkCancellation()
        guard frames > 0, frames <= 16384 else { throw AnalysisError.unsupportedAudioFormat }
        var block = [Int16](repeating: 0, count: frames * 2)
        let count = ta_decode_read(handle, &block, UInt64(frames))
        guard count >= 0 else { throw AnalysisError.decodingFailed("PCM decode failed") }
        return count == 0 ? nil : Array(block.prefix(Int(count) * 2))
    }
    func mono(_ range: Range<Int>) throws -> [Float] {
        guard range.lowerBound >= 0, range.upperBound <= frameCount,
              ta_decode_seek(handle, UInt64(range.lowerBound)) == 0 else {
            throw AnalysisError.decodingFailed("PCM seek failed")
        }
        var result = [Float](); result.reserveCapacity(range.count)
        while result.count < range.count, let block = try read(frames: min(16384, range.count - result.count)) {
            for index in 0..<min(block.count / 2, range.count - result.count) {
                result.append((Float(block[index * 2]) + Float(block[index * 2 + 1])) * (1 / 65536))
            }
        }
        guard result.count == range.count else { throw AnalysisError.decodingFailed("Truncated PCM") }
        return result
    }
}
