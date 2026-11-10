// swift-tools-version: 6.0
import PackageDescription

let package = Package(name: "TrackAnalysis", products: [
    .executable(name: "track-analysis-worker", targets: ["TrackAnalysisWorker"])
], targets: [
    .target(name: "PortableDSP", publicHeadersPath: "include",
            cSettings: [.unsafeFlags(["-O3"])]),
    .target(name: "Analysis", dependencies: ["PortableDSP"], exclude: ["Resources"]),
    .executableTarget(name: "TrackAnalysisWorker", dependencies: ["Analysis", "PortableDSP"]),
    .testTarget(name: "AnalysisTests", dependencies: ["Analysis", "PortableDSP"],
                resources: [.process("Fixtures")])
])
