import Foundation
import XCTest
@testable import Analysis

final class NativeBeatInferenceTests: XCTestCase {
    func testModelPathSelectionMatchesOriginalCallerAndWrapper() throws {
        // The original DatabaseMediator stores model7/model8 in this order;
        // original PCMAnalyze path-marker executions select x1 for normal and
        // x0 for dynamic. This checks the caller/wrapper composition that the
        // independently validated network weights cannot establish on their own.
        for dynamic in [false, true] {
            let plan = try NativeBeatInference.plan(sourceFrameCount: 2_646_001,
                sampleRate: 44_100, requestedOffset: 0, dynamic: dynamic)
            XCTAssertEqual(plan.modelVariant, dynamic ? .seven : .eight)
        }
    }

    func testModelWindowsAndEventRepairsMatchOriginalExecution() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "NativeBeatInference", withExtension: "json"))
        let fixture = try JSONDecoder().decode(Witnesses.self, from: Data(contentsOf: url))
        XCTAssertEqual(fixture.sourceSHA256, "2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907")
        for (index, witness) in fixture.cases.enumerated() {
            let plan = try NativeBeatInference.plan(sourceFrameCount: witness.sourceFrameCount,
                sampleRate: witness.sampleRate, requestedOffset: witness.requestedOffset, dynamic: witness.dynamic)
            XCTAssertEqual(plan.sourceRange.lowerBound, witness.expectedOffset, "window \(index)")
            XCTAssertEqual(plan.sourceRange.count, witness.expectedCount, "count \(index)")
            let corrected = NativeBeatInference.corrected(events: witness.events, sampleRate: witness.sampleRate,
                sampleCount: plan.sourceRange.count, dynamic: witness.dynamic)
            XCTAssertEqual(corrected, witness.corrected, "event correction \(index)")
        }
    }

    private struct Witnesses: Decodable {
        let sourceSHA256: String
        let cases: [Witness]
        struct Witness: Decodable {
            let sourceFrameCount: Int
            let sampleRate: Int
            let requestedOffset: Int
            let dynamic: Bool
            let events: [NativeBeatExport.Event]
            let expectedOffset: Int
            let expectedCount: Int
            let corrected: [NativeBeatExport.Event]
        }
    }
}
