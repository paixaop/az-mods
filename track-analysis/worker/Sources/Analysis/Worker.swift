/* SPDX-License-Identifier: GPL-3.0-only */
#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

public enum WorkerError: Error { case malformedGrid, noAnalysis, outOfRange }

public enum TrackAnalysisWorker {
    /// Returns legacy TA01 when needed is nil, or TA02 with the requested
    /// grid/phrase/vocal payloads and the decoded source clock otherwise.
    /// The core owns publication and checks the source again before saving.
    public static func analyze(audio: URL, savedDAT: URL?, globalBPM: Double, needed: UInt32? = nil) throws -> Data {
        let wantsPhrases = needed == nil || needed! & 8 != 0
        let wantsVocals = needed != nil && needed! & 32 != 0
        if let needed {
            guard needed & ~UInt32(4 | 8 | 32) == 0, wantsPhrases || wantsVocals,
                  needed & 4 == 0 || wantsPhrases else { throw WorkerError.outOfRange }
        }
        let input = try PCMDecoder(url: audio)
        let saved = try (wantsPhrases ? savedDAT : nil).flatMap { url -> [NativeBeatGrid.DetailedBeat]? in
            guard FileManager.default.fileExists(atPath: url.path) else { return nil }
            return try readGrid(Data(contentsOf: url), sampleRate: Double(input.rate))
        }
        let samples: NativePhraseFeatures.Samples
        let grid: [NativeBeatGrid.DetailedBeat]
        let timing: NativePhraseFeatures.Timing
        if !wantsPhrases || (saved != nil && !saved!.isEmpty) {
            let decoder = input
            let processor = NativePhrasePCMProcessor(sampleRate: Double(decoder.rate), channels: 2)
            var received = 0
            while let block = try decoder.read() {
                received += block.count / 2
                try processor.append(interleaved: block, frameCount: block.count / 2)
            }
            guard received == decoder.frameCount else { throw AnalysisError.decodingFailed("Frame count changed") }
            samples = processor.finish(declaredFrameCount: decoder.frameCount - 1)
            grid = saved ?? []
            if let saved, !saved.isEmpty {
                let bpm = globalBPM > 0 && globalBPM.isFinite ? globalBPM : Double(saved[0].bpm100) * 0.01
                timing = try NativePhraseFeatures.referenceTiming(bpm: bpm,
                    firstBeatTime: Double(saved[0].milliseconds) * 0.001,
                    firstBeatPosition: Int(saved[0].beatInBar), firstBeatTempoBPM: Double(saved[0].bpm100) * 0.01)
            } else { timing = .init(bpm: 120, offsetMilliseconds: 0, phase: 1) }
        } else {
            let result = try NativeBeatAnalyzer.analyze(url: audio)
            guard !result.grid.beats.isEmpty, result.grid.firstBeat.offsetMilliseconds >= 0,
                  (1...4).contains(result.grid.firstBeat.phase) else { throw WorkerError.noAnalysis }
            samples = result.phraseSamples; grid = result.grid.beats
            timing = .init(bpm: Double(result.grid.globalBPM100) * 0.01,
                offsetMilliseconds: Int(result.grid.firstBeat.offsetMilliseconds), phase: result.grid.firstBeat.phase)
        }
        var gridTag = Data(), phraseTag = Data(), vocalTag = Data()
        if wantsPhrases {
            let decision = try NativePhraseInference.classify(samples: samples, timing: timing, fixedGrid: true)
            guard !decision.output.entries.isEmpty, decision.output.endBeat <= grid.count,
                  decision.output.endBeat <= Int(UInt16.max), grid.count <= 32768,
                  decision.output.entries.count <= 1024 else { throw WorkerError.outOfRange }
            gridTag = saved == nil || saved!.isEmpty ? encodeGrid(grid) : Data()
            phraseTag = try encodePhrases(decision.output)
        }
        if wantsVocals { vocalTag = try VocalDetection.encode(samples: samples, sourceFrames: input.frameCount) }
        var result = Data((needed == nil ? "TA01" : "TA02").utf8)
        result.u32(gridTag.count); result.u32(phraseTag.count)
        if needed != nil { result.u32(vocalTag.count) }
        result.u32(input.rate)
        if needed != nil { result.u32(input.frameCount) }
        result.append(gridTag); result.append(phraseTag); result.append(vocalTag)
        return result
    }

    static func readGrid(_ data: Data, sampleRate: Double) throws -> [NativeBeatGrid.DetailedBeat]? {
        guard data.count >= 12, data.count <= 16 * 1024 * 1024,
              data.prefix(4) == Data("PMAI".utf8), data.be32(8) == data.count else { throw WorkerError.malformedGrid }
        var at = data.be32(4), result: [NativeBeatGrid.DetailedBeat]?
        guard at >= 12, at <= data.count else { throw WorkerError.malformedGrid }
        while at < data.count {
            guard data.count - at >= 12 else { throw WorkerError.malformedGrid }
            let header = data.be32(at + 4), length = data.be32(at + 8)
            guard header >= 12, header <= length, length <= data.count - at else { throw WorkerError.malformedGrid }
            if data[at..<(at + 4)] == Data("PQTZ".utf8) {
                guard result == nil, header >= 24 else { throw WorkerError.malformedGrid }
                let count = data.be32(at + 20)
                guard count <= 32768, count <= (length - header) / 8 else { throw WorkerError.malformedGrid }
                var beats = [NativeBeatGrid.DetailedBeat]()
                for index in 0..<count {
                    let row = at + header + index * 8
                    let beat = data.be16(row), bpm = data.be16(row + 2), ms = data.be32(row + 4)
                    guard (1...4).contains(beat), bpm > 0 else { throw WorkerError.malformedGrid }
                    if let previous = beats.last {
                        let order = compare_timestamps(Double(ms), Double(previous.milliseconds), 1000, sampleRate, 1)
                        guard order != AZ_TIME_INVALID, order == AZ_TIME_AFTER else { throw WorkerError.malformedGrid }
                    }
                    beats.append(.init(beatInBar: UInt16(beat), bpm100: UInt16(bpm), milliseconds: UInt32(ms), microsecondRemainder: 0))
                }
                result = beats
            }
            at += length
        }
        return result
    }
    static func encodeGrid(_ grid: [NativeBeatGrid.DetailedBeat]) -> Data {
        var tag = Data("PQTZ".utf8); tag.u32(24); tag.u32(24 + grid.count * 8)
        tag.u32(0); tag.u32(0x80000); tag.u32(grid.count)
        for beat in grid { tag.u16(Int(beat.beatInBar)); tag.u16(Int(beat.bpm100)); tag.u32(Int(beat.milliseconds)) }
        return tag
    }
    static func encodePhrases(_ output: NativePhraseExport.Result) throws -> Data {
        let mood = output.mood == .high ? 1 : output.mood == .mid ? 2 : 3
        var tag = Data("PSSI".utf8); tag.u32(32); tag.u32(32 + 24 * output.entries.count)
        tag.u16(0); tag.u16(24); tag.u16(output.entries.count); tag.u16(mood)
        tag.append(contentsOf: repeatElement(UInt8(0), count: 6)); tag.u16(output.endBeat); tag.u32(0)
        for row in output.entries {
            let fields = [row.index, row.startBeat, row.code.kind, row.beat2, row.beat3, row.beat4, row.fillStartBeat]
            let flags = [row.code.k1, row.code.k2, row.code.k3, row.b, row.fill]
            guard fields.allSatisfy({ (0...65535).contains($0) }),
                  flags.allSatisfy({ (0...255).contains($0) }) else { throw WorkerError.outOfRange }
            tag.u16(row.index); tag.u16(row.startBeat); tag.u16(row.code.kind)
            tag.append(contentsOf: [0, UInt8(row.code.k1), 0, UInt8(row.code.k2), 0, UInt8(row.b)])
            tag.u16(row.beat2); tag.u16(row.beat3); tag.u16(row.beat4)
            tag.append(contentsOf: [0, UInt8(row.code.k3), 0, UInt8(row.fill)])
            tag.u16(row.fillStartBeat)
        }
        return tag
    }
}

let AZ_TIME_INVALID = 2, AZ_TIME_EQUAL = 0, AZ_TIME_AFTER = 1, AZ_TIME_BEFORE = -1
func compare_timestamps(_ first: Double, _ second: Double, _ units: UInt64, _ rate: Double, _ window: UInt64) -> Int {
    guard first.isFinite, second.isFinite, units > 0, rate.isFinite, rate > 0, window > 0 else { return AZ_TIME_INVALID }
    if abs(first - second) * Double(rate) < Double(window) * Double(units) { return AZ_TIME_EQUAL }
    return first < second ? AZ_TIME_BEFORE : AZ_TIME_AFTER
}
extension Data {
    func be16(_ at: Int) -> Int { Int(self[at]) << 8 | Int(self[at + 1]) }
    func be32(_ at: Int) -> Int { be16(at) << 16 | be16(at + 2) }
    mutating func u16(_ value: Int) { append(UInt8(truncatingIfNeeded: value >> 8)); append(UInt8(truncatingIfNeeded: value)) }
    mutating func u32(_ value: Int) { u16(value >> 16); u16(value) }
}
