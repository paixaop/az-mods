#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Beat candidate shared by the streaming detector, tempo selection and envelope
/// adjustment. Periods remain Float milliseconds until final grid conversion.
struct NativeBeatInfo: Equatable, Sendable {
    var sampleIndex: Int
    var intervalInBeats: Float
    var periodMilliseconds: Float
    var beatInBar: Int
    var sourceBand: Int
    var flag: UInt8

    var sampleBeat: NativeBeatGrid.SampleBeat {
        .init(sampleIndex: Int32(sampleIndex), beatInBar: Int32(beatInBar),
              bpm100: Int32(Float(6_000_000) / periodMilliseconds + 0.5))
    }
}
