import Foundation

/// JSONSerialization's Linux decimal NSNumber conversion can move a value by
/// one ULP. Decode IEEE Double directly and retain NSNumber's Int/Double bridge.
private enum FixtureJSON: Decodable {
    case object([String: FixtureJSON]), array([FixtureJSON]), number(Double)
    case string(String), bool(Bool), null
    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let n = try? c.decode(Double.self) { self = .number(n) }
        else if let s = try? c.decode(String.self) { self = .string(s) }
        else if let a = try? c.decode([FixtureJSON].self) { self = .array(a) }
        else { self = .object(try c.decode([String: FixtureJSON].self)) }
    }
    var value: Any {
        switch self {
        case .object(let d): d.mapValues(\.value)
        case .array(let a): a.map(\.value)
        case .number(let n): NSNumber(value: n)
        case .string(let s): s
        case .bool(let b): b
        case .null: NSNull()
        }
    }
}
func fixtureJSONObject(with data: Data) throws -> Any {
    try JSONDecoder().decode(FixtureJSON.self, from: data).value
}
