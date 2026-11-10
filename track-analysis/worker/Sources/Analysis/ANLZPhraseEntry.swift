#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

public struct ANLZPhraseEntry: Sendable, Codable, Equatable {
    public let index: Int
    public let startBeat: Int
    public let code: RekordboxPhraseCode
    public let b: Int
    public let beat2: Int
    public let beat3: Int
    public let beat4: Int
    /// Raw flag and raw one-based position are retained even when fill == 0.
    public let fill: Int
    public let fillStartBeat: Int
}
