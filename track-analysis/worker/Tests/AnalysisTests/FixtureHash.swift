import Foundation
#if canImport(CryptoKit)
import CryptoKit
#endif

/// Keep all frozen byte checks enabled on Linux without linking crypto into the worker.
func fixtureSHA256(_ data: Data) throws -> String {
    #if canImport(CryptoKit)
    return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    #else
    let process = Process(), input = Pipe(), output = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/sha256sum")
    process.standardInput = input; process.standardOutput = output
    try process.run()
    input.fileHandleForWriting.write(data)
    try input.fileHandleForWriting.close()
    let result = output.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    guard process.terminationStatus == 0,
          let text = String(data: result, encoding: .utf8),
          let digest = text.split(separator: " ").first, digest.count == 64 else {
        throw NSError(domain: "FixtureHash", code: 1)
    }
    return String(digest)
    #endif
}
