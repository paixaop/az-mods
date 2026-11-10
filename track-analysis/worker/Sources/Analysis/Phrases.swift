#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

public enum PhraseMood: String, CaseIterable, Codable, Sendable {
    case high, mid, low
}

public enum PhraseKind: String, CaseIterable, Codable, Sendable {
    case intro, up, down, chorus, bridge, verse, outro, unknown
    public var displayName: String { rawValue.prefix(1).uppercased() + rawValue.dropFirst() }
    public var supportsFillIn: Bool { self == .intro || self == .up || self == .chorus }
}

/// Variant numbers describe a mood-specific style, never the occurrence count in a song.
public struct PhraseLabel: Codable, Sendable, Equatable, Hashable {
    public let kind: PhraseKind
    public let variant: Int?
    public init(kind: PhraseKind, variant: Int? = nil) {
        self.kind = kind
        self.variant = variant
    }
    var isStructurallyValid: Bool {
        guard let variant else { return true }
        switch kind {
        case .intro, .chorus, .outro: return (1...2).contains(variant)
        case .up: return (1...3).contains(variant)
        case .verse: return (1...6).contains(variant)
        case .down, .bridge, .unknown: return false
        }
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(kind: try c.decode(PhraseKind.self, forKey: .kind), variant: try c.decodeIfPresent(Int.self, forKey: .variant))
        guard isStructurallyValid else { throw PhraseError.invalidLabel }
    }
    private enum CodingKeys: String, CodingKey { case kind, variant }
    public var displayName: String { kind.displayName + (variant.map { " \($0)" } ?? "") }

    public static func available(in mood: PhraseMood) -> [PhraseLabel] {
        switch mood {
        case .high:
            return [Self(kind: .intro, variant: 1), Self(kind: .intro, variant: 2),
                    Self(kind: .up, variant: 1), Self(kind: .up, variant: 2), Self(kind: .up, variant: 3),
                    Self(kind: .down), Self(kind: .chorus, variant: 1), Self(kind: .chorus, variant: 2),
                    Self(kind: .outro, variant: 1), Self(kind: .outro, variant: 2)]
        case .mid:
            return [Self(kind: .intro)] + (1...6).map { Self(kind: .verse, variant: $0) }
                + [Self(kind: .chorus), Self(kind: .bridge), Self(kind: .outro)]
        case .low:
            return [Self(kind: .intro), Self(kind: .verse, variant: 1), Self(kind: .verse, variant: 2),
                    Self(kind: .chorus), Self(kind: .bridge), Self(kind: .outro)]
        }
    }

    /// Audio inference may establish a family without establishing its numbered style.
    public func isValid(in mood: PhraseMood) -> Bool {
        guard isStructurallyValid else { return false }
        if kind == .unknown { return variant == nil }
        return Self.available(in: mood).contains { $0.kind == kind && (variant == nil || $0.variant == variant) }
    }
}

public enum PhraseSource: String, Codable, Sendable { case inferredAudio, importedMetadata, manual }

public enum PhraseFillInDetection: String, Codable, Sendable { case snareRoll, cut, mixed, transientPattern }
public enum PhraseTimingSource: String, Codable, Sendable { case independentBeats, externalReference, importedMetadata }

public struct PhraseStyleEvidence: Codable, Sendable, Equatable {
    public let variant: Int
    public let confidence: Double
    public let evidence: [String]
    public let warnings: [String]
    public init(variant: Int, confidence: Double, evidence: [String], warnings: [String] = []) throws {
        guard (1...6).contains(variant), confidence.isFinite, (0...1).contains(confidence), !evidence.isEmpty else {
            throw PhraseError.invalidLabel
        }
        self.variant = variant; self.confidence = confidence; self.evidence = evidence; self.warnings = warnings
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(variant: c.decode(Int.self, forKey: .variant), confidence: c.decode(Double.self, forKey: .confidence),
                      evidence: c.decode([String].self, forKey: .evidence), warnings: c.decodeIfPresent([String].self, forKey: .warnings) ?? [])
    }
    private enum CodingKeys: String, CodingKey { case variant, confidence, evidence, warnings }
}

public struct PhraseFillIn: Codable, Sendable, Equatable {
    public let start: Double
    public let end: Double
    public let beatCount: Double
    public let detection: PhraseFillInDetection?
    public let evidence: [String]?
    public let confidence: Double
    public init(start: Double, end: Double, beatCount: Double, confidence: Double, detection: PhraseFillInDetection? = nil, evidence: [String]? = nil) {
        self.start = start; self.end = end; self.beatCount = beatCount; self.confidence = confidence
        self.detection = detection; self.evidence = evidence
    }
    var isValid: Bool {
        start.isFinite && end.isFinite && start >= 0 && start < end
            && beatCount.isFinite && beatCount > 0 && beatCount <= 4
            && confidence.isFinite && (0...1).contains(confidence)
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(start: try c.decode(Double.self, forKey: .start), end: try c.decode(Double.self, forKey: .end),
                  beatCount: try c.decode(Double.self, forKey: .beatCount), confidence: try c.decode(Double.self, forKey: .confidence),
                  detection: try c.decodeIfPresent(PhraseFillInDetection.self, forKey: .detection), evidence: try c.decodeIfPresent([String].self, forKey: .evidence))
        guard isValid else { throw PhraseError.invalidFillIn }
    }
    private enum CodingKeys: String, CodingKey { case start, end, beatCount, confidence, detection, evidence }
}

/// Retains decoded identifiers, including unrecognized values, for lossless label inspection.
public struct RekordboxPhraseCode: Codable, Sendable, Equatable {
    public let kind: Int
    public let k1: Int
    public let k2: Int
    public let k3: Int
    public init(kind: Int, k1: Int = 0, k2: Int = 0, k3: Int = 0) {
        self.kind = kind; self.k1 = k1; self.k2 = k2; self.k3 = k3
    }
    public func label(in mood: PhraseMood) -> PhraseLabel {
        switch mood {
        case .high:
            switch kind {
            case 1: return PhraseLabel(kind: .intro, variant: k1 == 1 ? 1 : k1 == 0 ? 2 : nil)
            case 2:
                let variant = k2 == 0 && k3 == 0 ? 1 : k2 == 0 && k3 == 1 ? 2 : k2 == 1 && k3 == 0 ? 3 : nil
                return PhraseLabel(kind: .up, variant: variant)
            case 3: return PhraseLabel(kind: .down)
            case 5: return PhraseLabel(kind: .chorus, variant: k1 == 1 ? 1 : k1 == 0 ? 2 : nil)
            case 6: return PhraseLabel(kind: .outro, variant: k1 == 1 ? 1 : k1 == 0 ? 2 : nil)
            default: return PhraseLabel(kind: .unknown)
            }
        case .mid, .low:
            switch kind {
            case 1: return PhraseLabel(kind: .intro)
            case 2...7: return PhraseLabel(kind: .verse, variant: mood == .mid ? kind - 1 : kind <= 4 ? 1 : 2)
            case 8: return PhraseLabel(kind: .bridge)
            case 9: return PhraseLabel(kind: .chorus)
            case 10: return PhraseLabel(kind: .outro)
            default: return PhraseLabel(kind: .unknown)
            }
        }
    }
}

/// An inferred material identity, independent of display style and occurrence number.
public struct PhraseRecurrenceEvidence: Codable, Sendable, Equatable {
    public let groupID: Int
    public let similarity: Double
    public init(groupID: Int, similarity: Double) throws {
        guard groupID >= 0, similarity.isFinite, (0...1).contains(similarity) else { throw PhraseError.invalidLabel }
        self.groupID = groupID; self.similarity = similarity
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(groupID: c.decode(Int.self, forKey: .groupID), similarity: c.decode(Double.self, forKey: .similarity))
    }
}

public struct TrackPhrase: Codable, Sendable, Equatable {
    /// Half-open source interval [start, end), in seconds.
    public let start: Double
    public let end: Double
    public let label: PhraseLabel
    public let confidence: Double
    public let source: PhraseSource
    public let fillIn: PhraseFillIn?
    public let referenceCode: RekordboxPhraseCode?
    public let styleEvidence: PhraseStyleEvidence?
    public let recurrence: PhraseRecurrenceEvidence?
    public init(start: Double, end: Double, label: PhraseLabel, confidence: Double,
                source: PhraseSource, fillIn: PhraseFillIn? = nil, referenceCode: RekordboxPhraseCode? = nil, styleEvidence: PhraseStyleEvidence? = nil, recurrence: PhraseRecurrenceEvidence? = nil) throws {
        guard start.isFinite, end.isFinite, start >= 0, end > start,
              confidence.isFinite, (0...1).contains(confidence) else { throw PhraseError.invalidInterval }
        guard label.isStructurallyValid else { throw PhraseError.invalidLabel }
        if let styleEvidence {
            guard PhraseLabel(kind: label.kind, variant: styleEvidence.variant).isStructurallyValid,
                  label.variant == nil || label.variant == styleEvidence.variant else { throw PhraseError.invalidLabel }
        }
        if let fillIn {
            guard fillIn.isValid, label.kind.supportsFillIn, fillIn.start >= start,
                  abs(fillIn.end - end) < 1e-7 else { throw PhraseError.invalidFillIn }
        }
        self.start = start; self.end = end; self.label = label; self.confidence = confidence
        self.source = source; self.fillIn = fillIn; self.referenceCode = referenceCode; self.styleEvidence = styleEvidence; self.recurrence = recurrence
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(start: c.decode(Double.self, forKey: .start), end: c.decode(Double.self, forKey: .end),
                      label: c.decode(PhraseLabel.self, forKey: .label), confidence: c.decode(Double.self, forKey: .confidence),
                      source: c.decode(PhraseSource.self, forKey: .source),
                      fillIn: c.decodeIfPresent(PhraseFillIn.self, forKey: .fillIn),
                      referenceCode: c.decodeIfPresent(RekordboxPhraseCode.self, forKey: .referenceCode),
                      styleEvidence: c.decodeIfPresent(PhraseStyleEvidence.self, forKey: .styleEvidence), recurrence: c.decodeIfPresent(PhraseRecurrenceEvidence.self, forKey: .recurrence))
    }
    private enum CodingKeys: String, CodingKey { case start, end, label, confidence, source, fillIn, referenceCode, styleEvidence, recurrence }
}

/// Bounded counters from phrase decision stages; no source audio is retained.
/// `recordedCounters` distinguishes measured zero from an uninstrumented legacy field.
public struct PhraseDiagnostics: Codable, Sendable, Equatable {
    /// Names of counters measured by this run. Nil identifies legacy results
    /// that did not declare counter availability.
    public var recordedCounters: Set<String>?
    public var measuredTimingCount = 0
    public var interpolatedTimingCount = 0
    public var unsupportedTimingCount = 0
    public var proposedBoundaryCount = 0
    public var refinedBoundaryCount = 0
    public var selectedBoundaryCount = 0
    public var evaluatedFillCount = 0
    public var acousticFillCandidateCount = 0
    public var fillFamilyRejectionCount = 0
    public var fillBoundsRejectionCount = 0
    public var emittedFillCount = 0
    public var bandLevelObservationCount = 0
    public var sourceEnvelopeObservationCount = 0
    public var provisionalFillCount = 0
    public var fillMoodRejectionCount = 0
    public var fillCleanupCount = 0
    public var fillContextRejectionCount = 0

    public init() {}
}

extension PhraseDiagnostics {
    private enum CodingKeys: String, CodingKey { case recordedCounters, measuredTimingCount, interpolatedTimingCount, unsupportedTimingCount, proposedBoundaryCount, refinedBoundaryCount, selectedBoundaryCount, evaluatedFillCount, acousticFillCandidateCount, fillFamilyRejectionCount, fillBoundsRejectionCount, emittedFillCount, bandLevelObservationCount, sourceEnvelopeObservationCount, provisionalFillCount, fillMoodRejectionCount, fillCleanupCount, fillContextRejectionCount }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init()
        recordedCounters = try c.decodeIfPresent(Set<String>.self, forKey: .recordedCounters)
        measuredTimingCount = try c.decodeIfPresent(Int.self, forKey: .measuredTimingCount) ?? 0
        interpolatedTimingCount = try c.decodeIfPresent(Int.self, forKey: .interpolatedTimingCount) ?? 0
        unsupportedTimingCount = try c.decodeIfPresent(Int.self, forKey: .unsupportedTimingCount) ?? 0
        proposedBoundaryCount = try c.decodeIfPresent(Int.self, forKey: .proposedBoundaryCount) ?? 0
        refinedBoundaryCount = try c.decodeIfPresent(Int.self, forKey: .refinedBoundaryCount) ?? 0
        selectedBoundaryCount = try c.decodeIfPresent(Int.self, forKey: .selectedBoundaryCount) ?? 0
        evaluatedFillCount = try c.decodeIfPresent(Int.self, forKey: .evaluatedFillCount) ?? 0
        acousticFillCandidateCount = try c.decodeIfPresent(Int.self, forKey: .acousticFillCandidateCount) ?? 0
        fillFamilyRejectionCount = try c.decodeIfPresent(Int.self, forKey: .fillFamilyRejectionCount) ?? 0
        fillBoundsRejectionCount = try c.decodeIfPresent(Int.self, forKey: .fillBoundsRejectionCount) ?? 0
        emittedFillCount = try c.decodeIfPresent(Int.self, forKey: .emittedFillCount) ?? 0
        bandLevelObservationCount = try c.decodeIfPresent(Int.self, forKey: .bandLevelObservationCount) ?? 0
        sourceEnvelopeObservationCount = try c.decodeIfPresent(Int.self, forKey: .sourceEnvelopeObservationCount) ?? 0
        provisionalFillCount = try c.decodeIfPresent(Int.self, forKey: .provisionalFillCount) ?? 0
        fillMoodRejectionCount = try c.decodeIfPresent(Int.self, forKey: .fillMoodRejectionCount) ?? 0
        fillCleanupCount = try c.decodeIfPresent(Int.self, forKey: .fillCleanupCount) ?? 0
        fillContextRejectionCount = try c.decodeIfPresent(Int.self, forKey: .fillContextRejectionCount) ?? 0
        guard [measuredTimingCount, interpolatedTimingCount, unsupportedTimingCount, proposedBoundaryCount, refinedBoundaryCount, selectedBoundaryCount, evaluatedFillCount, acousticFillCandidateCount, fillFamilyRejectionCount, fillBoundsRejectionCount, emittedFillCount, bandLevelObservationCount, sourceEnvelopeObservationCount, provisionalFillCount, fillMoodRejectionCount, fillCleanupCount, fillContextRejectionCount].allSatisfy({ $0 >= 0 }) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Negative phrase diagnostic count."))
        }
    }
}

public struct PhraseAnalysis: Codable, Sendable, Equatable {
    public let mood: PhraseMood?
    /// Legacy heuristic confidence. Version 5 leaves this nil because the
    /// recovered classifier does not produce a calibrated probability.
    public let moodConfidence: Double?
    public let rawMoodID: Int?
    public let phrases: [TrackPhrase]
    public let timingSource: PhraseTimingSource
    public let warnings: [String]
    public let diagnostics: PhraseDiagnostics?
    public let context: PhraseContextEvidence?
    public init(mood: PhraseMood?, moodConfidence: Double? = nil, rawMoodID: Int? = nil, phrases: [TrackPhrase], timingSource: PhraseTimingSource? = nil, warnings: [String] = [], diagnostics: PhraseDiagnostics? = nil, context: PhraseContextEvidence? = nil) throws {
        if let moodConfidence {
            guard mood != nil, moodConfidence.isFinite, (0...1).contains(moodConfidence) else { throw PhraseError.invalidMood }
        }
        for (index, phrase) in phrases.enumerated() {
            if let mood, !phrase.label.isValid(in: mood) { throw PhraseError.invalidLabel }
            if let mood, let style = phrase.styleEvidence,
               !PhraseLabel(kind: phrase.label.kind, variant: style.variant).isValid(in: mood) { throw PhraseError.invalidLabel }
            if index > 0, phrases[index - 1].end > phrase.start { throw PhraseError.invalidInterval }
        }
        self.mood = mood; self.moodConfidence = moodConfidence; self.rawMoodID = rawMoodID; self.phrases = phrases
        self.timingSource = timingSource ?? (rawMoodID == nil ? .independentBeats : .importedMetadata)
        self.warnings = warnings; self.diagnostics = diagnostics; self.context = context
    }
    public var unknownDuration: Double { phrases.filter { $0.label.kind == .unknown }.reduce(0) { $0 + $1.end - $1.start } }
    public func phrase(at time: Double) -> TrackPhrase? {
        phrases.first { time >= $0.start && time < $0.end }
    }
    public func phrases(ofKind kind: PhraseKind) -> [TrackPhrase] { phrases.filter { $0.label.kind == kind } }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(mood: c.decodeIfPresent(PhraseMood.self, forKey: .mood),
                      moodConfidence: c.decodeIfPresent(Double.self, forKey: .moodConfidence),
                      rawMoodID: c.decodeIfPresent(Int.self, forKey: .rawMoodID), phrases: c.decode([TrackPhrase].self, forKey: .phrases),
                      timingSource: c.decodeIfPresent(PhraseTimingSource.self, forKey: .timingSource), warnings: c.decodeIfPresent([String].self, forKey: .warnings) ?? [],
                      diagnostics: c.decodeIfPresent(PhraseDiagnostics.self, forKey: .diagnostics), context: c.decodeIfPresent(PhraseContextEvidence.self, forKey: .context))
    }
    private enum CodingKeys: String, CodingKey { case mood, moodConfidence, rawMoodID, phrases, timingSource, warnings, diagnostics, context }
}

public enum PhraseError: Error, Sendable {
    case invalidInterval, invalidFillIn, invalidMood, invalidLabel, invalidBeatGrid
    case destinationExists, unsupportedDestination
}
