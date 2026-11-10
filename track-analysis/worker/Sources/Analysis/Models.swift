#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Automatic preserves the detector's tempo-variation decision. Normal forces
/// the recovered constant-tempo path.
public enum RhythmMode: String, Codable, Sendable, Equatable { case automatic, normal }

public enum SpectralBackend: String, Codable, Sendable, Equatable {
    case automatic, metal, accelerate
}

public struct AnalysisConfiguration: Codable, Sendable, Equatable {
    public var spectralBackend: SpectralBackend
    public var detectRhythm: Bool
    public var minimumBPM: Double
    public var maximumBPM: Double
    public var inferDownbeats: Bool
    public var beatsPerBar: Int
    public var rhythmMode: RhythmMode
    /// Nil estimates mood from audio; an explicit mood constrains the phrase vocabulary.
    public var phraseMood: PhraseMood?
    public var inferPhraseStyles: Bool
    public var detectFillIns: Bool
    public init(detectRhythm: Bool = true, minimumBPM: Double = 60, maximumBPM: Double = 200,
                inferDownbeats: Bool = false, beatsPerBar: Int = 4,
                rhythmMode: RhythmMode = .automatic, spectralBackend: SpectralBackend = .automatic,
                phraseMood: PhraseMood? = nil, detectFillIns: Bool = true, inferPhraseStyles: Bool = true) {
        self.spectralBackend = spectralBackend
        self.detectRhythm = detectRhythm
        self.minimumBPM = minimumBPM
        self.maximumBPM = maximumBPM
        self.inferDownbeats = inferDownbeats
        self.beatsPerBar = beatsPerBar
        self.rhythmMode = rhythmMode
        self.phraseMood = phraseMood
        self.detectFillIns = detectFillIns
        self.inferPhraseStyles = inferPhraseStyles
    }
}

public struct TempoCandidate: Codable, Sendable, Equatable {
    public let bpm: Double
    public let confidence: Double
}
public struct TempoEstimate: Codable, Sendable, Equatable {
    public let bpm: Double
    public let confidence: Double
    /// Empty when the algorithm does not expose competing metrical interpretations.
    public let candidates: [TempoCandidate]
}
public struct Beat: Codable, Sendable, Equatable {
    /// Position in the native grid; indices survive playback-clock endpoint trimming.
    public let index: Int
    public let time: Double
    public let confidence: Double
}
public struct Downbeat: Codable, Sendable, Equatable {
    public let time: Double
    /// Uncalibrated confidence for the four-beat model and audio phase reconciliation.
    public let inferredConfidence: Double
}
public enum SectionLabel: String, Codable, Sendable, Equatable {
    case intro, buildUp, drop, breakdown, outro, unknown
}
public struct TrackSection: Codable, Sendable, Equatable {
    public let start: Double
    public let end: Double
    public let label: SectionLabel
    public let confidence: Double
}
public struct EnergyPoint: Codable, Sendable, Equatable {
    public let time: Double
    public let rms: Double
}
public struct AnalysisDiagnostics: Codable, Sendable, Equatable {
    public let requestedSpectralBackend: SpectralBackend
    public let actualSpectralBackend: SpectralBackend
    public let spectralBackendReason: String
    public let gpuDeviceName: String?
    public let spectralBatchSize: Int
    public let spectralBatchCount: Int
    public let messages: [String]
    public let featureFrameCount: Int
    public let featureRate: Double
    public let onsetCount: Int
    public let rhythmMode: RhythmMode
}
public struct AnalysisResult: Codable, Sendable, Equatable {
    public let duration: Double
    public let sampleRate: Double
    public let tempo: TempoEstimate?
    public let beats: [Beat]
    public let downbeats: [Downbeat]
    public let sections: [TrackSection]
    public let energyCurve: [EnergyPoint]
    public let diagnostics: AnalysisDiagnostics
    public let version: String
    /// Nil for cached results written before phrase analysis was introduced.
    public var phraseAnalysis: PhraseAnalysis? = nil
    public var acousticCurve: [AcousticPoint]? = nil
    public var sourceEnvelope: SourceEnvelopeCurve? = nil
}
public enum AnalysisError: Error, Sendable {
    case invalidConfiguration(String)
    case metalUnavailable(String)
    case metalExecutionFailed(String)
    case invalidSamples
    case unsupportedAudioFormat
    case decodingFailed(String)
}

extension AnalysisConfiguration {
    private enum CodingKeys: String, CodingKey {
        case spectralBackend, detectRhythm, minimumBPM, maximumBPM, inferDownbeats, beatsPerBar, rhythmMode
        case phraseMood, detectFillIns, inferPhraseStyles
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        self.init(detectRhythm: try values.decodeIfPresent(Bool.self, forKey: .detectRhythm) ?? true,
                  minimumBPM: try values.decodeIfPresent(Double.self, forKey: .minimumBPM) ?? 60,
                  maximumBPM: try values.decodeIfPresent(Double.self, forKey: .maximumBPM) ?? 200,
                  inferDownbeats: try values.decodeIfPresent(Bool.self, forKey: .inferDownbeats) ?? false,
                  beatsPerBar: try values.decodeIfPresent(Int.self, forKey: .beatsPerBar) ?? 4,
                  rhythmMode: try values.decodeIfPresent(RhythmMode.self, forKey: .rhythmMode) ?? .automatic,
                  spectralBackend: try values.decodeIfPresent(SpectralBackend.self, forKey: .spectralBackend) ?? .automatic,
                  phraseMood: try values.decodeIfPresent(PhraseMood.self, forKey: .phraseMood),
                  detectFillIns: try values.decodeIfPresent(Bool.self, forKey: .detectFillIns) ?? true,
                  inferPhraseStyles: try values.decodeIfPresent(Bool.self, forKey: .inferPhraseStyles) ?? true)
    }
}
extension AnalysisDiagnostics {
    private enum CodingKeys: String, CodingKey {
        case requestedSpectralBackend, actualSpectralBackend, spectralBackendReason, gpuDeviceName
        case spectralBatchSize, spectralBatchCount, messages, featureFrameCount, featureRate, onsetCount, rhythmMode
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        self.init(requestedSpectralBackend: try values.decodeIfPresent(SpectralBackend.self, forKey: .requestedSpectralBackend) ?? .automatic,
                  actualSpectralBackend: try values.decodeIfPresent(SpectralBackend.self, forKey: .actualSpectralBackend) ?? .accelerate,
                  spectralBackendReason: try values.decodeIfPresent(String.self, forKey: .spectralBackendReason) ?? "Legacy result used the Accelerate backend.",
                  gpuDeviceName: try values.decodeIfPresent(String.self, forKey: .gpuDeviceName),
                  spectralBatchSize: try values.decodeIfPresent(Int.self, forKey: .spectralBatchSize) ?? 0,
                  spectralBatchCount: try values.decodeIfPresent(Int.self, forKey: .spectralBatchCount) ?? 0,
                  messages: try values.decode([String].self, forKey: .messages),
                  featureFrameCount: try values.decode(Int.self, forKey: .featureFrameCount),
                  featureRate: try values.decode(Double.self, forKey: .featureRate),
                  onsetCount: try values.decode(Int.self, forKey: .onsetCount),
                  rhythmMode: try values.decode(RhythmMode.self, forKey: .rhythmMode))
    }
}

/// Reduced-rate rich acoustic observations. Drum values are band-limited attack activity,
/// not isolated instrument stems. Bands span 30...5512 Hz; chroma has C...B pitch classes.
public struct AcousticPoint: Codable, Sendable, Equatable {
    public let time: Double
    public let bands: [Double]
    public let chroma: [Double]
    public let centroidHz: Double
    public let flatness: Double
    public let lowKick: Double
    public let midSnare: Double
    public let highHat: Double
    public let relativeDynamics: Double
}

/// A caller-supplied complete timing reference for phrase comparison only. It never replaces
/// the independently measured tempo, beats or downbeats in AnalysisResult.
public struct PhraseTimingReference: Codable, Sendable, Equatable {
    public let beatTimes: [Double]
    public let beatPositions: [Int]?
    public let beatsPerBar: Int
    /// The grid producer's global tempo, separate from individual beat tempos.
    public let tempoBPM: Double?
    /// The first recorded beat's instantaneous tempo, when provided by ANLZ.
    /// This is used only by the reference caller's initial offset/phase rule.
    public let firstBeatTempoBPM: Double?
    public init(beatTimes: [Double], beatPositions: [Int]? = nil, beatsPerBar: Int = 4,
                tempoBPM: Double? = nil, firstBeatTempoBPM: Double? = nil) throws {
        guard beatTimes.count >= 2, (2...12).contains(beatsPerBar),
              beatTimes.allSatisfy({ $0.isFinite && $0 >= 0 }),
              zip(beatTimes, beatTimes.dropFirst()).allSatisfy({ $0 < $1 }) else { throw PhraseError.invalidBeatGrid }
        if let beatPositions {
            guard beatPositions.count == beatTimes.count,
                  beatPositions.allSatisfy({ (1...beatsPerBar).contains($0) }),
                  zip(beatPositions, beatPositions.dropFirst()).allSatisfy({ $1 == $0 % beatsPerBar + 1 }) else {
                throw PhraseError.invalidBeatGrid
            }
        }
        if let tempoBPM, !tempoBPM.isFinite || tempoBPM <= 0 { throw PhraseError.invalidBeatGrid }
        if let firstBeatTempoBPM, !firstBeatTempoBPM.isFinite || firstBeatTempoBPM <= 0 { throw PhraseError.invalidBeatGrid }
        self.beatTimes = beatTimes; self.beatPositions = beatPositions; self.beatsPerBar = beatsPerBar
        self.tempoBPM = tempoBPM
        self.firstBeatTempoBPM = firstBeatTempoBPM
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(beatTimes: c.decode([Double].self, forKey: .beatTimes),
                      beatPositions: c.decodeIfPresent([Int].self, forKey: .beatPositions),
                      beatsPerBar: c.decodeIfPresent(Int.self, forKey: .beatsPerBar) ?? 4,
                      tempoBPM: c.decodeIfPresent(Double.self, forKey: .tempoBPM),
                      firstBeatTempoBPM: c.decodeIfPresent(Double.self, forKey: .firstBeatTempoBPM))
    }
    private enum CodingKeys: String, CodingKey { case beatTimes, beatPositions, beatsPerBar, tempoBPM, firstBeatTempoBPM }
}

extension AcousticPoint {
    private enum CodingKeys: String, CodingKey {
        case time, bands, chroma, centroidHz, flatness, lowKick, midSnare, highHat, relativeDynamics
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(time: c.decode(Double.self, forKey: .time), bands: c.decode([Double].self, forKey: .bands),
                  chroma: c.decode([Double].self, forKey: .chroma), centroidHz: c.decode(Double.self, forKey: .centroidHz),
                  flatness: c.decode(Double.self, forKey: .flatness), lowKick: c.decode(Double.self, forKey: .lowKick),
                  midSnare: c.decode(Double.self, forKey: .midSnare), highHat: c.decode(Double.self, forKey: .highHat),
                  relativeDynamics: c.decode(Double.self, forKey: .relativeDynamics))
        guard time.isFinite, time >= 0, bands.count == 8, chroma.count == 12,
              (bands + chroma + [centroidHz, lowKick, midSnare, highHat]).allSatisfy({ $0.isFinite && $0 >= 0 }),
              flatness.isFinite, (0...1).contains(flatness), relativeDynamics.isFinite else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Invalid rich acoustic observation."))
        }
    }
}
