#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Source-rate, causal envelope evidence. Bands are independent percussion proxies.
public struct SourceEnvelopePoint: Codable, Sendable, Equatable {
    public let time: Double
    public let levels: [Double]
    public let peaks: [Double]
    /// Mean-square / squared peak: a crest-based occupancy summary, not onset count.
    public let occupancy: [Double]
    /// Smoothed low/middle/upper envelope maxima, absent in older caches.
    public var causalPeaks: [Double]? = nil
}
public struct SourceEnvelopeCurve: Codable, Sendable, Equatable {
    public let sampleRate: Double
    public let minimumFrequencies: [Double]
    public let availableBands: [Bool]
    public let points: [SourceEnvelopePoint]
    public var causalAvailableBands: [Bool]? = nil
    public var causalMinimumFrequencies: [Double]? = nil
    public var causalMaximumFrequencies: [Double]? = nil
    public var causalSampleRates: [Double]? = nil
    public var isCausalBandEnvelope: Bool { causalAvailableBands != nil && causalMinimumFrequencies != nil && causalMaximumFrequencies != nil && causalSampleRates != nil }
}
