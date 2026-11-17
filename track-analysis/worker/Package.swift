// swift-tools-version: 6.0
import PackageDescription

let package = Package(name: "TrackAnalysis", products: [
    .executable(name: "track-analysis-worker", targets: ["TrackAnalysisWorker"])
], targets: [
    .target(name: "FAAD", exclude: ["COPYING", "SOURCE.md"], publicHeadersPath: "include",
            cSettings: [.headerSearchPath("libfaad"),
                        .define("LC_ONLY_DECODER"),
                        .define("HAVE_INTTYPES_H", to: "1"), .define("HAVE_MEMCPY", to: "1"),
                        .define("HAVE_STRING_H", to: "1"), .define("HAVE_STRINGS_H", to: "1"),
                        .define("HAVE_SYS_STAT_H", to: "1"), .define("HAVE_SYS_TYPES_H", to: "1"),
                        .define("HAVE_LRINTF", to: "1"), .define("PACKAGE_VERSION", to: "\"2.11.4\""),
                        .unsafeFlags(["-O3"])]),
    .target(name: "PortableDSP", dependencies: ["FAAD"],
            exclude: ["SOURCE.md", "MINIMP4-LICENSE"], publicHeadersPath: "include",
            cSettings: [.unsafeFlags(["-O3"])]),
    .target(name: "Analysis", dependencies: ["PortableDSP"], exclude: ["Resources"]),
    .executableTarget(name: "TrackAnalysisWorker", dependencies: ["Analysis", "PortableDSP"]),
    .testTarget(name: "AnalysisTests", dependencies: ["Analysis", "PortableDSP"],
                resources: [.process("Fixtures")])
])
