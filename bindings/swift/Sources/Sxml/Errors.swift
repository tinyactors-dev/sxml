// The errors of the Swift package (D238): SxmlError for a failure in sxml,
// SxmlUsageError for a misuse of the package. Golden:
// bindings/javascript/src/index.ts, "Errors".

/// The status of a failure (include/sxml/status.h), by the names of
/// `sxml_status` without the `SXML_STATUS_` prefix.
public enum Status: String, Sendable, CaseIterable {
    case ok = "OK"
    case invalidArgument = "INVALID_ARGUMENT"
    case insufficientMemory = "INSUFFICIENT_MEMORY"
    case misalignedMemory = "MISALIGNED_MEMORY"
    case capacityExceeded = "CAPACITY_EXCEEDED"
    case limitExceeded = "LIMIT_EXCEEDED"
    case outOfMemory = "OUT_OF_MEMORY"
    case arithmeticOverflow = "ARITHMETIC_OVERFLOW"
    case indexOutOfRange = "INDEX_OUT_OF_RANGE"
    case unexpectedEndOfInput = "UNEXPECTED_END_OF_INPUT"
    case malformedInput = "MALFORMED_INPUT"
    case invalidUTF8 = "INVALID_UTF8"
    case unsupportedFeature = "UNSUPPORTED_FEATURE"
    case inputOutputFailed = "INPUT_OUTPUT_FAILED"
    case staticError = "STATIC_ERROR"
    case dynamicError = "DYNAMIC_ERROR"
    case typeError = "TYPE_ERROR"
    /// A name this package does not know (the set is closed; it cannot
    /// happen unless the library is newer than the package).
    case unknown = "UNKNOWN"

    /// The name as `sxml_status_name` gives it without its prefix, such
    /// as "DYNAMIC_ERROR".
    public var name: String { rawValue }
}

/// A failure in sxml: a static, dynamic or type error of an expression
/// (with its error code), a document that is not well-formed, a limit, or
/// out of memory.
public struct SxmlError: Error, CustomStringConvertible, CustomDebugStringConvertible, Sendable {
    public let status: Status
    /// The error code as prefix:local, such as "err:FOAR0001"; "" when the
    /// status has none.
    public let code: String
    public let codeNamespaceURI: String
    public let codeLocalName: String
    /// sxml's description of the failure ("" when it has none). This is
    /// also the `CustomStringConvertible` text; `message` has the code and
    /// location too.
    public let description: String
    /// The module or document the location is in: "expression", "query",
    /// a module's URI; "" when unknown.
    public let sourceName: String
    public let lineNumber: Int
    public let columnNumber: Int

    /// "line:column: code: description", as the JavaScript package's
    /// message.
    public var message: String {
        let place = lineNumber > 0 ? "\(lineNumber):\(columnNumber): " : ""
        let what = description.isEmpty ? status.name : description
        return place + (code.isEmpty ? "" : code + ": ") + what
    }

    public var debugDescription: String { "SxmlError(\(message))" }

    init(status: Status, codeNamespaceURI: String, codeLocalName: String, codePrefix: String,
         description: String, sourceName: String, lineNumber: Int, columnNumber: Int) {
        self.status = status
        if codeLocalName.isEmpty {
            code = ""
        } else if !codePrefix.isEmpty {
            code = codePrefix + ":" + codeLocalName
        } else if !codeNamespaceURI.isEmpty {
            code = "Q{" + codeNamespaceURI + "}" + codeLocalName
        } else {
            code = codeLocalName
        }
        self.codeNamespaceURI = codeNamespaceURI
        self.codeLocalName = codeLocalName
        self.description = description
        self.sourceName = sourceName
        self.lineNumber = lineNumber
        self.columnNumber = columnNumber
    }

    /// A failed allocation (the package's own or sxml's).
    static let outOfMemory = SxmlError(
        status: .outOfMemory, codeNamespaceURI: "", codeLocalName: "", codePrefix: "",
        description: "Out of memory", sourceName: "", lineNumber: 0, columnNumber: 0)
}

/// A misuse of the package rather than a failure in sxml.
public enum SxmlUsageError: Error, CustomStringConvertible, Sendable, Equatable {
    /// A resolver called its own processor (D71: callbacks never recurse
    /// into the processor that runs them).
    case resolverUsesProcessor
    /// A value made by another processor was given to this one.
    case valueOfAnotherProcessor
    /// A feature name that is not one of `Sxml.features`.
    case unknownFeature(String)
    /// An input that is none of the types `XdmInput` lists, or one
    /// `fn:parse-json` cannot read (a number that is not finite, a value
    /// inside an array or map).
    case unsupportedInput(String)

    public var description: String {
        switch self {
        case .resolverUsesProcessor:
            return "sxml: a resolver must not use its processor"
        case .valueOfAnotherProcessor:
            return "sxml: the value belongs to another processor"
        case .unknownFeature(let name):
            return "sxml: this library has no optional feature '\(name)'"
        case .unsupportedInput(let what):
            return "sxml: cannot convert \(what) to a value"
        }
    }
}
