#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

public enum PhraseBoundaryPolicy: String, Codable, Sendable, Equatable { case activity, texture }

/// Independent context scores. These are not calibrated commercial mood definitions.
public struct PhraseContextEvidence: Codable, Sendable, Equatable {
    public let tentativeMood: PhraseMood
    public let revisedMood: PhraseMood
    public let boundaryPolicy: PhraseBoundaryPolicy
    public let percussiveActivity: Double
    public let sustainedActivity: Double
    public let relativeDynamics: Double
    public init(tentativeMood: PhraseMood, revisedMood: PhraseMood, boundaryPolicy: PhraseBoundaryPolicy,
                percussiveActivity: Double, sustainedActivity: Double, relativeDynamics: Double) throws {
        guard [percussiveActivity, sustainedActivity, relativeDynamics].allSatisfy({ $0.isFinite && (0...1).contains($0) }) else { throw PhraseError.invalidMood }
        self.tentativeMood = tentativeMood; self.revisedMood = revisedMood; self.boundaryPolicy = boundaryPolicy
        self.percussiveActivity = percussiveActivity; self.sustainedActivity = sustainedActivity; self.relativeDynamics = relativeDynamics
    }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(tentativeMood: c.decode(PhraseMood.self, forKey: .tentativeMood), revisedMood: c.decode(PhraseMood.self, forKey: .revisedMood),
                      boundaryPolicy: c.decode(PhraseBoundaryPolicy.self, forKey: .boundaryPolicy), percussiveActivity: c.decode(Double.self, forKey: .percussiveActivity),
                      sustainedActivity: c.decode(Double.self, forKey: .sustainedActivity), relativeDynamics: c.decode(Double.self, forKey: .relativeDynamics))
    }
}
