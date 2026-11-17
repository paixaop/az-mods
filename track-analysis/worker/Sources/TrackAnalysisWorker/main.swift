/* SPDX-License-Identifier: GPL-3.0-only */
#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP
import Analysis

do {
    let args = CommandLine.arguments
    guard (args.count == 6 || args.count == 7), let bpm = Double(args[3]) else {
        throw WorkerError.outOfRange
    }
    let needed: UInt32? = args.count == 7 ? UInt32(args[6]) : nil
    if args.count == 7 && needed == nil { throw WorkerError.outOfRange }
    let result = try TrackAnalysisWorker.analyze(audio: URL(fileURLWithPath: args[1]),
        savedDAT: args[2] == "-" ? nil : URL(fileURLWithPath: args[2]), globalBPM: bpm, needed: needed)
    try result.write(to: URL(fileURLWithPath: args[4]), options: [.atomic])
} catch {
    let message = Array("Track Analysis failed: \(error)\n".utf8)
    message.withUnsafeBytes { _ = write(2, $0.baseAddress, $0.count) }
    exit(1)
}
