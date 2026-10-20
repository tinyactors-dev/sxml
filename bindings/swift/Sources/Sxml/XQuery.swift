// XQuery 4.0 with the Update Facility 3.0 (include/sxml/xquery.h; D216,
// D238). Golden: bindings/javascript/src/index.ts (compileQuery, XQuery,
// deliverUpdates).
internal import CSxmlHost

/// A tree an updating query changed (XQuery Update Facility 3.0,
/// upd:applyUpdates): trees never change in sxml, so the update makes a
/// new tree; putting it where the original was is the host's.
public struct UpdatedTree {
    /// The root as the query read it: a document node you bound, or a
    /// parentless node.
    public let original: Value
    /// The root of the new tree with every change made.
    public let updated: Value
}

/// A document fn:put stores (XQUF 3.0, 6.1): writing it at uri is the
/// host's.
public struct PutDocument {
    /// fn:put's $uri resolved against the static base URI.
    public let uri: String
    public let node: Value
    /// fn:put's $params, or nil.
    public let parameters: Value?

    /// The node serialized with parameters (method xml without).
    public func serialize() throws -> String {
        let processor = node.processor
        if let parameters {
            return try processor.serializeWith(node, options: parameters)
        }
        let empty: [String: (any XdmInput)?] = [:]
        return try processor.serializeWith(node, options: try processor.value(empty))
    }
}

/// Receives what an updating query changed, after the evaluation and in
/// the order of each tree's first update; documents fn:put stores come
/// last. Without putDocument, fn:put raises FOUP0002. What a closure does
/// not take is released.
public struct UpdateSink {
    public var updatedTree: ((UpdatedTree) throws -> Void)?
    public var putDocument: ((PutDocument) throws -> Void)?

    public init(updatedTree: ((UpdatedTree) throws -> Void)? = nil,
                putDocument: ((PutDocument) throws -> Void)? = nil)
    {
        self.updatedTree = updatedTree
        self.putDocument = putDocument
    }
}

extension Processor {
    /// Compiles an XQuery 4.0 main module, with the library modules it
    /// imports (the resolver is asked for them, kind `.xqueryModule`), for
    /// evaluating it many times.
    /// - Parameters:
    ///   - namespaces: prefix to URI, added to XQuery's predeclared
    ///     prefixes and to those of the processor's features; the prolog
    ///     may rebind them.
    ///   - baseURI: the static base URI, against which module locations
    ///     and fn:doc resolve.
    public func compileQuery(_ query: String, namespaces: [String: String] = [:],
                             baseURI: String? = nil) throws -> XQuery
    {
        try enter()
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        let namespaceBindings = try self.namespaceBindings(namespaces, scratch: &scratch)
        var request = sxml_host_query_compile_request(
            query_text: try scratch.view(query),
            static_base_uri: try scratch.view(optional: baseURI),
            namespaces: try scratch.array(namespaceBindings),
            namespace_count: namespaceBindings.count)
        guard let compiled = sxml_host_query_compile(handle, &request) else {
            throw failure()
        }
        return XQuery(processor: self, handle: compiled)
    }
}

/// A compiled XQuery 4.0 main module (include/sxml/xquery.h). It keeps its
/// processor alive.
public final class XQuery {
    public let processor: Processor
    /// Whether its body is an updating expression (XQUF 3.0, 2.2): its
    /// result is then empty and its changes go to the update sink.
    public let isUpdating: Bool
    private let handle: OpaquePointer

    init(processor: Processor, handle: OpaquePointer) {
        self.processor = processor
        self.handle = handle
        isUpdating = sxml_host_query_is_updating(handle) != 0
    }

    deinit {
        sxml_host_query_destroy(handle)
    }

    /// Evaluates the query; the result is a new value.
    /// - Parameters:
    ///   - context: the initial context value; nil means none (XPDY0002
    ///     when the query needs it).
    ///   - variables: the external variables the prolog declares (XQuery
    ///     4.0, 5.16), by NCName or Q{uri}local; names it does not declare
    ///     are ignored, and a declared one without a value takes its
    ///     default.
    ///   - updates: for an updating query, receives the changed trees and
    ///     fn:put's documents after the evaluation.
    ///   - messages: receives fn:trace and fn:message output after the
    ///     evaluation, also when it throws.
    public func evaluate(context: (any XdmInput)? = nil,
                         variables: [String: (any XdmInput)?] = [:],
                         updates: UpdateSink? = nil,
                         messages: ((Message) throws -> Void)? = nil) throws -> Value
    {
        try processor.enter()
        var scratch = Scratch(processor: processor.handle)
        defer { scratch.release() }
        var made: [Value] = []
        let contextValue = try context.map { try processor.input($0, made: &made) }
        var request = sxml_host_query_evaluate_request(
            context_value: contextValue?.handle,
            variables: try processor.bindingList(variables, scratch: &scratch, made: &made),
            accepts_put_documents: updates?.putDocument == nil ? 0 : 1,
            side_results: noSideResults)
        let result = withExtendedLifetime((contextValue, made)) {
            sxml_host_query_evaluate(handle, &request)
        }
        // Read before a callback can use the processor.
        let failure = result == nil ? processor.failure() : nil
        let sideResults = processor.sideResults(request.side_results)
        let value = result.map { Value(processor: processor, handle: $0) }
        try processor.deliverMessages(sideResults, to: messages)
        try deliverUpdates(sideResults, to: updates)
        guard let value else {
            throw failure ?? SxmlError.outOfMemory
        }
        return value
    }

    /// Serializes a value with the serialization parameters the query's
    /// output declarations give (XQuery 4.0, 5.22: method xml unless they
    /// name another). `Value.serialize(_:)` takes its own options instead.
    public func serialize(_ value: Value) throws -> String {
        try processor.enter()
        try processor.check(value)
        let status = withExtendedLifetime(value) {
            sxml_host_query_serialize(handle, value.handle)
        }
        if status != SXML_STATUS_OK.rawValue {
            throw processor.failure()
        }
        return processor.output()
    }

    /// Evaluates the query and serializes its result as `serialize(_:)`
    /// does.
    public func run(context: (any XdmInput)? = nil, variables: [String: (any XdmInput)?] = [:],
                    updates: UpdateSink? = nil,
                    messages: ((Message) throws -> Void)? = nil) throws -> String
    {
        try serialize(
            try evaluate(context: context, variables: variables, updates: updates, messages: messages))
    }

    /// Hands the UPDATED_TREE and PUT_DOCUMENT side results to the sink;
    /// what it does not take is released with the side results.
    private func deliverUpdates(_ results: [SideResult], to sink: UpdateSink?) throws {
        for result in results {
            if result.kind == SideResult.updatedTree, let original = result.first,
                let updated = result.second
            {
                try sink?.updatedTree?(UpdatedTree(original: original, updated: updated))
            } else if result.kind == SideResult.putDocument, let node = result.first {
                try sink?.putDocument?(PutDocument(uri: result.uri, node: node, parameters: result.second))
            }
        }
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension XQuery: Sendable {}
