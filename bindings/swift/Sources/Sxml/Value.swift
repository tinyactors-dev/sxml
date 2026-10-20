// Values and compiled XPath expressions (D238). Golden:
// bindings/javascript/src/index.ts (Value, XPathExpression).
internal import CSxmlHost

/// An XDM value: a sequence of items in its processor (a parsed document,
/// an evaluation's result). It keeps its processor alive.
public final class Value: CustomStringConvertible {
    public let processor: Processor
    let handle: OpaquePointer

    init(processor: Processor, handle: OpaquePointer) {
        self.processor = processor
        self.handle = handle
    }

    deinit {
        sxml_host_value_release(handle)
    }

    /// Serializes the value (`Processor.serialize`): by the adaptive method
    /// without options.
    public func serialize(_ options: [String: (any XdmInput)?]? = nil) throws -> String {
        try processor.serialize(self, options: options)
    }

    /// The value by the adaptive method: items one per line, strings
    /// quoted, nodes as XML. When that fails, the failure's text.
    public var description: String {
        do {
            return try serialize()
        } catch let error as SxmlError {
            return error.message
        } catch {
            return "\(error)"
        }
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension Value: Sendable {}

/// A compiled XPath 4.0 expression (include/sxml/xpath.h). It keeps its
/// processor alive.
public final class XPathExpression {
    public let processor: Processor
    /// The external variables, in the order compile was given them.
    public let variables: [String]
    private let handle: OpaquePointer

    init(processor: Processor, handle: OpaquePointer, variables: [String]) {
        self.processor = processor
        self.handle = handle
        self.variables = variables
    }

    deinit {
        sxml_host_expression_destroy(handle)
    }

    /// Evaluates the expression; the result is a new value.
    /// - Parameters:
    ///   - context: the context value; nil means no focus (XPDY0002 when
    ///     used).
    ///   - variables: values of the declared variables; a missing one
    ///     raises XPDY0002, and nil is the empty sequence.
    public func evaluate(context: (any XdmInput)? = nil,
                         variables bindings: [String: (any XdmInput)?] = [:]) throws -> Value
    {
        try processor.enter()
        var made: [Value] = []
        let contextValue = try context.map { try processor.input($0, made: &made) }
        var values: [Value?] = []
        for name in variables {
            if let binding = bindings[name] {
                values.append(try processor.input(binding, made: &made))
            } else {
                values.append(nil)
            }
        }
        return try withExtendedLifetime(made) {
            try processor.evaluateHandle(handle, context: contextValue, variableValues: values)
        }
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension XPathExpression: Sendable {}
