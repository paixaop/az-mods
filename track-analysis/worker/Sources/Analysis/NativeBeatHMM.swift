#if canImport(FoundationEssentials)
import FoundationEssentials
#else
import Foundation
#endif
import PortableDSP

/// Beat/downbeat state decoding used by the audio-only timing analysis.
enum NativeBeatHMM {
    struct Event: Codable, Equatable, Sendable {
        let seconds: Float
        let beatInBar: Int
    }

    struct Configuration: Equatable, Sendable {
        let beatsPerBar = [4]
        let minimumBPM = 70.0
        let maximumBPM = 180.0
        let numberOfTempi = 60
        let transitionLambda = 100
        let observationLambda = 16
        var threshold: Float = 0.05
        var correct = true
        let framesPerSecond = 100.0

        static let native = Configuration()
    }

    enum InputError: Error {
        case invalidConfiguration
        case invalidActivations
        case excessiveStateSpace
    }

    struct Model {
        let positions: [Double]
        let intervals: [Int]
        let observations: [Int]
        let previous: [Int]
        let pointers: [Int]
        let probabilities: [Double]
        let firstStateSlots: [Int]
        let firstStateCount: Int
    }

    struct Path {
        let states: [Int]
        let logProbability: Double
    }

    /// Activations are interleaved `[beat, downbeat]` probabilities at 100 Hz.
    static func track(
        activations: [Float],
        configuration: Configuration = .native
    ) throws -> [Event] {
        try Task.checkCancellation()
        try validate(configuration)
        guard activations.count.isMultiple(of: 2),
              activations.allSatisfy({ $0.isFinite && $0 >= 0 && $0 <= 1 }) else {
            throw InputError.invalidActivations
        }
        let frameCount = activations.count / 2
        guard frameCount > 0 else { return [] }
        var first = 0
        var end = frameCount
        if configuration.threshold != 0 {
            guard let leading = (0..<frameCount).first(where: {
                activations[$0 * 2] >= configuration.threshold ||
                    activations[$0 * 2 + 1] >= configuration.threshold
            }), let trailing = (0..<frameCount).last(where: {
                activations[$0 * 2] >= configuration.threshold ||
                    activations[$0 * 2 + 1] >= configuration.threshold
            }) else { return [] }
            first = leading
            end = trailing + 1
        }
        let input = Array(activations[(first * 2)..<(end * 2)])
        guard input.contains(where: { $0 != 0 }) else { return [] }
        let densities = logDensities(input, observationLambda: configuration.observationLambda)
        var selectedModel: Model?
        var selectedPath: Path?
        for beatsPerBar in configuration.beatsPerBar {
            let model = try makeModel(beatsPerBar: beatsPerBar, configuration: configuration)
            let path = try viterbi(densities: densities, model: model)
            if selectedPath == nil || path.logProbability > selectedPath!.logProbability {
                selectedModel = model
                selectedPath = path
            }
        }
        guard let model = selectedModel, let path = selectedPath, !path.states.isEmpty else {
            return []
        }
        let beatNumbers = path.states.map { Int(model.positions[$0]) + 1 }
        var frames: [Int] = []
        if configuration.correct {
            let beatRange = path.states.map { model.observations[$0] >= 1 }
            // The native wrapper returns before endpoint insertion when the
            // beat-region mask has no transitions, including a one-frame crop.
            guard zip(beatRange, beatRange.dropFirst()).contains(where: { $0 != $1 }) else { return [] }
            var frame = 0
            while frame < path.states.count {
                guard beatRange[frame] else {
                    frame += 1
                    continue
                }
                let start = frame
                repeat { frame += 1 } while frame < path.states.count && beatRange[frame]
                var maximum = start * 2
                for index in (start * 2 + 1)..<(frame * 2) where input[index] > input[maximum] {
                    maximum = index
                }
                frames.append(maximum / 2)
            }
        } else if beatNumbers.count > 1 {
            for frame in 1..<beatNumbers.count where beatNumbers[frame] != beatNumbers[frame - 1] {
                frames.append(frame)
            }
        }
        let secondsPerFrame = 1 / configuration.framesPerSecond
        return frames.map {
            Event(seconds: Float(Double($0 + first) * secondsPerFrame), beatInBar: beatNumbers[$0])
        }
    }

    static func logDensities(_ activations: [Float], observationLambda: Int) -> [Float] {
        let reciprocal = 1 / (Float(observationLambda) - 1)
        var result = [Float]()
        result.reserveCapacity(activations.count / 2 * 3)
        for frame in 0..<(activations.count / 2) {
            let beat = activations[frame * 2]
            let downbeat = activations[frame * 2 + 1]
            // Round a Double logarithm once to Float. Platform logf routines
            // otherwise disagree by one Float ULP on the frozen activations.
            result.append(Float(log(Double((1 - (beat + downbeat)) * reciprocal))))
            result.append(Float(log(Double(beat))))
            result.append(Float(log(Double(downbeat))))
        }
        return result
    }

    static func makeModel(beatsPerBar: Int, configuration: Configuration) throws -> Model {
        try validate(configuration)
        let minimum = configuration.framesPerSecond * 60 / configuration.maximumBPM
        let maximum = configuration.framesPerSecond * 60 / configuration.minimumBPM
        let lower = Int(minimum.rounded(.toNearestOrEven))
        let upper = Int(maximum.rounded(.toNearestOrEven))
        guard lower > 0, upper >= lower, upper <= 100_000, beatsPerBar > 0 else {
            throw InputError.invalidConfiguration
        }
        guard beatsPerBar == 4 else { throw InputError.invalidConfiguration }
        // The confirmed settings have 54 integer intervals, below the 60-tempo cap.
        let intervals = Array(lower...upper)
        let statesPerBeat = intervals.reduce(0, +)
        guard statesPerBeat <= 1_000_000 / beatsPerBar else { throw InputError.excessiveStateSpace }
        let stateCount = statesPerBeat * beatsPerBar
        let intervalCount = intervals.count
        var ends: [Int] = []
        var offset = 0
        for interval in intervals {
            offset += interval
            ends.append(offset - 1)
        }
        var kernel = [[Double]]()
        if lower == 33, upper == 86, configuration.transitionLambda == 100 {
            var width = 0
            guard let values = ta_hmm_kernel(&width), width == intervalCount else { throw InputError.invalidConfiguration }
            kernel = (0..<width).map { Array(UnsafeBufferPointer(start: values + $0 * width, count: width)) }
        } else {
          for from in intervals {
            var row = intervals.map { to -> Double in
                guard configuration.transitionLambda != 0 else { return from == to ? 1 : 0 }
                let value = exp(-Double(configuration.transitionLambda) * abs(Double(to) / Double(from) - 1))
                return value <= Double.ulpOfOne ? 0 : value
            }
            // The reference stores the public model's precomputed transition
            // table. Its normalization uses NumPy's pairwise summation order.
            let sum = pairwiseSum(row)
            for index in row.indices { row[index] /= sum }
            kernel.append(row)
          }
        }
        var positions = [Double]()
        var stateIntervals = [Int]()
        var observations = [Int]()
        var previous = [Int]()
        var probabilities = [Double]()
        var pointers = [0]
        var firstStateSlots = [Int](repeating: -1, count: stateCount)
        positions.reserveCapacity(stateCount)
        stateIntervals.reserveCapacity(stateCount)
        observations.reserveCapacity(stateCount)
        pointers.reserveCapacity(stateCount + 1)
        let beatWidth = 1 / Double(configuration.observationLambda)
        for beat in 0..<beatsPerBar {
            let previousBeat = (beat + beatsPerBar - 1) % beatsPerBar
            for (intervalIndex, interval) in intervals.enumerated() {
                let step = 1 / Double(interval)
                for subframe in 0..<interval {
                    let state = positions.count
                    let position = Double(subframe) * step + Double(beat)
                    positions.append(position)
                    stateIntervals.append(interval)
                    observations.append(position < beatWidth ? 2 :
                        (position.truncatingRemainder(dividingBy: 1) < beatWidth ? 1 : 0))
                    if subframe == 0 {
                        firstStateSlots[state] = beat * intervalCount + intervalIndex
                        for source in intervals.indices where kernel[source][intervalIndex] != 0 {
                            previous.append(previousBeat * statesPerBeat + ends[source])
                            probabilities.append(kernel[source][intervalIndex])
                        }
                    } else {
                        previous.append(state - 1)
                        probabilities.append(1)
                    }
                    pointers.append(previous.count)
                }
            }
        }
        return Model(positions: positions, intervals: stateIntervals, observations: observations,
                     previous: previous, pointers: pointers, probabilities: probabilities,
                     firstStateSlots: firstStateSlots, firstStateCount: beatsPerBar * intervalCount)
    }

    static func viterbi(densities: [Float], model: Model) throws -> Path {
        let stateCount = model.positions.count
        let frameCount = densities.count / 3
        guard stateCount > 0, frameCount > 0 else { return Path(states: [], logProbability: -.infinity) }
        let transitionLogs = model.probabilities.map { log($0) }
        var previous = [Double](repeating: log(1 / Double(stateCount)), count: stateCount)
        var current = [Double](repeating: -.infinity, count: stateCount)
        // Within an interval the predecessor is always state - 1. Only interval
        // starts need stored choices, reducing a minute's traceback to about 5 MB.
        var traceback = [UInt32](repeating: 0, count: frameCount * model.firstStateCount)
        for frame in 0..<frameCount {
            if frame.isMultiple(of: 64) { try Task.checkCancellation() }
            for state in 0..<stateCount {
                let density = Double(densities[frame * 3 + model.observations[state]])
                if model.firstStateSlots[state] < 0 {
                    let candidate = previous[state - 1] + density
                    current[state] = candidate > -.infinity ? candidate : -.infinity
                } else {
                    var best = -Double.infinity
                    var source = 0
                    for transition in model.pointers[state]..<model.pointers[state + 1] {
                        let candidate = (previous[model.previous[transition]] + density) + transitionLogs[transition]
                        if candidate > best {
                            best = candidate
                            source = model.previous[transition]
                        }
                    }
                    current[state] = best
                    traceback[frame * model.firstStateCount + model.firstStateSlots[state]] = UInt32(source)
                }
            }
            swap(&current, &previous)
        }
        var final = 0
        for state in 1..<stateCount where previous[state] > previous[final] { final = state }
        let probability = previous[final]
        var path = [Int](repeating: 0, count: frameCount)
        for frame in stride(from: frameCount - 1, through: 0, by: -1) {
            path[frame] = final
            let slot = model.firstStateSlots[final]
            final = slot < 0 ? final - 1 : Int(traceback[frame * model.firstStateCount + slot])
        }
        return Path(states: path, logProbability: probability)
    }

    private static func validate(_ configuration: Configuration) throws {
        guard !configuration.beatsPerBar.isEmpty, configuration.beatsPerBar.allSatisfy({ $0 > 0 && $0 <= 32 }),
              configuration.minimumBPM.isFinite, configuration.minimumBPM > 0,
              configuration.maximumBPM.isFinite, configuration.maximumBPM >= configuration.minimumBPM,
              configuration.framesPerSecond.isFinite, configuration.framesPerSecond > 0,
              configuration.numberOfTempi == 0 || configuration.numberOfTempi >= 2,
              configuration.transitionLambda >= 0, configuration.observationLambda > 1,
              configuration.threshold.isFinite, configuration.threshold >= 0 else {
            throw InputError.invalidConfiguration
        }
    }

    private static func pairwiseSum(_ values: [Double]) -> Double {
        var lanes = [Double](repeating: 0, count: 8)
        let vectorEnd = values.count / 8 * 8
        for index in 0..<vectorEnd { lanes[index % 8] += values[index] }
        var sum = ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
            ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]))
        for index in vectorEnd..<values.count { sum += values[index] }
        return sum
    }
}
