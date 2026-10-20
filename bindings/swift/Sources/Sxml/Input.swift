// Input (D138, D238): what a context, a variable or `Processor.value`
// takes. A Value of the processor is used as it is, a String is an
// xs:string, and other data is converted as fn:parse-json converts its JSON
// text (F&O 4.0, 17.5.1: objects are maps, arrays are arrays, numbers
// xs:double, null the empty sequence). Golden:
// bindings/javascript/src/index.ts, Processor.value.

/// A value to bind: a `Value` of the processor, a `String`, or data
/// fn:parse-json reads: `Bool`, `Int`, `Double`, `[XdmInput?]`,
/// `[String: XdmInput?]`, and `nil` (the empty sequence). Other types
/// that conform are refused (`SxmlUsageError.unsupportedInput`).
public protocol XdmInput {}

extension Value: XdmInput {}
extension String: XdmInput {}
extension Bool: XdmInput {}
extension Int: XdmInput {}
extension Double: XdmInput {}
extension Array: XdmInput where Element == (any XdmInput)? {}
extension Dictionary: XdmInput where Key == String, Value == (any XdmInput)? {}

/// The JSON text of data, for fn:parse-json. Builds it with an explicit
/// stack, so that nesting depth costs no call depth.
func jsonText(of data: (any XdmInput)?) throws -> String {
    // Each entry is data still to write, or text to write as it is.
    enum Pending {
        case data((any XdmInput)?)
        case text(String)
    }
    var output = ""
    var pending: [Pending] = [.data(data)]
    while let next = pending.popLast() {
        switch next {
        case .text(let text):
            output += text
        case .data(let item):
            guard let item else {
                output += "null"
                continue
            }
            switch item {
            case let string as String:
                appendJSONString(string, to: &output)
            case let boolean as Bool:
                output += boolean ? "true" : "false"
            case let integer as Int:
                output += String(integer)
            case let double as Double:
                guard double.isFinite else {
                    throw SxmlUsageError.unsupportedInput("the number \(double)")
                }
                output += String(double)
            case let array as [(any XdmInput)?]:
                // Pushed in reverse, so that they pop in order.
                pending.append(.text("]"))
                for (position, member) in array.enumerated().reversed() {
                    pending.append(.data(member))
                    if position > 0 {
                        pending.append(.text(","))
                    }
                }
                pending.append(.text("["))
            case let map as [String: (any XdmInput)?]:
                pending.append(.text("}"))
                for (position, entry) in map.sorted(by: { $0.key < $1.key }).enumerated().reversed() {
                    pending.append(.data(entry.value))
                    var key = ""
                    appendJSONString(entry.key, to: &key)
                    pending.append(.text((position > 0 ? "," : "") + key + ":"))
                }
                pending.append(.text("{"))
            case is Value:
                throw SxmlUsageError.unsupportedInput("a Value inside an array or map")
            default:
                throw SxmlUsageError.unsupportedInput("a \(type(of: item))")
            }
        }
    }
    return output
}

/// Appends string as a JSON string (RFC 8259, 7).
private func appendJSONString(_ string: String, to output: inout String) {
    output += "\""
    for scalar in string.unicodeScalars {
        switch scalar {
        case "\"":
            output += "\\\""
        case "\\":
            output += "\\\\"
        case _ where scalar.value < 0x20:
            let hexadecimal = String(scalar.value, radix: 16, uppercase: true)
            output += "\\u" + String(repeating: "0", count: 4 - hexadecimal.count) + hexadecimal
        default:
            output.unicodeScalars.append(scalar)
        }
    }
    output += "\""
}
