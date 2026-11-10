#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Input from an existing ANLZ/PSSI decoder. Beat numbers are one-based, including fillStartBeat.
/// This adapter deliberately does not parse binary files or implement the ANLZ XOR envelope.
public struct RekordboxPhraseEntry: Sendable {
    public let startBeat: Int
    public let code: RekordboxPhraseCode
    public let fillStartBeat: Int?
    public init(startBeat: Int, code: RekordboxPhraseCode, fillStartBeat: Int? = nil) {
        self.startBeat = startBeat; self.code = code; self.fillStartBeat = fillStartBeat
    }
}

extension PhraseAnalysis {
    /// `beatTimes` must include each referenced boundary, including `endBeat`; no constant-tempo
    /// extrapolation is used. The sparse inferred `AnalysisResult.beats` is not a full PQTZ grid.
    public static func fromRekordbox(moodID: Int, entries: [RekordboxPhraseEntry], endBeat: Int,
                                    beatTimes: [Double]) throws -> PhraseAnalysis {
        guard beatTimes.allSatisfy({ $0.isFinite && $0 >= 0 }),
              zip(beatTimes, beatTimes.dropFirst()).allSatisfy({ $0 < $1 }),
              endBeat > 0, endBeat <= beatTimes.count else { throw PhraseError.invalidBeatGrid }
        let mood: PhraseMood? = switch moodID { case 1: .high; case 2: .mid; case 3: .low; default: nil }
        var phrases = [TrackPhrase]()
        for index in entries.indices {
            let entry = entries[index]
            let end = index + 1 < entries.count ? entries[index + 1].startBeat : endBeat
            guard entry.startBeat > 0, entry.startBeat < end, end <= endBeat else { throw PhraseError.invalidBeatGrid }
            let label = mood.map { entry.code.label(in: $0) } ?? PhraseLabel(kind: .unknown)
            let startTime = beatTimes[entry.startBeat - 1], endTime = beatTimes[end - 1]
            var fill: PhraseFillIn?
            if let fillStart = entry.fillStartBeat {
                guard fillStart >= entry.startBeat, fillStart < end, end - fillStart <= 4,
                      label.kind.supportsFillIn else { throw PhraseError.invalidFillIn }
                fill = PhraseFillIn(start: beatTimes[fillStart - 1], end: endTime,
                                   beatCount: Double(end - fillStart), confidence: 1)
            }
            phrases.append(try TrackPhrase(start: startTime, end: endTime, label: label,
                confidence: label.kind == .unknown ? 0 : 1, source: .importedMetadata,
                fillIn: fill, referenceCode: entry.code))
        }
        return try PhraseAnalysis(mood: mood, rawMoodID: moodID, phrases: phrases)
    }
}
