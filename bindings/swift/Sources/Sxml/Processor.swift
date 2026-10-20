// The Swift package's processor (D238): module Sxml over the host shim
// (bindings/host/sxml_host.h), imported through the module map CSxmlHost
// with internal import, so that no C type is in the module's interface.
// Golden: bindings/javascript/src/index.ts (Sxml, Processor).
//
// Memory is ARC: every object that owns memory in the shim (Processor,
// XPathExpression, Value) releases it in deinit, and every object retains
// the processor that made it, so a processor is destroyed after the last
// of them. Threads (D35): a processor is used by one thread at a time, so
// none of these types is Sendable, and Swift 6 strict concurrency rejects
// sharing one across isolation domains at compile time.
internal import CSxmlHost

/// The library: its version and its optional features.
public enum Sxml {
    /// The library version, such as "0.2.0".
    public static let version = String(cString: sxml_host_version_text())

    /// The optional features the library has (include/sxml/processor.h,
    /// "Optional features"; D189): every one but EXPath File ("test" adds
    /// nothing; it checks that the list reaches the processor).
    public static let features: [String] = (0..<sxml_host_feature_count()).map { featureIndex in
        let bytes = sxml_host_feature_name_bytes(featureIndex)
        let byteCount = Int(sxml_host_feature_name_byte_count(featureIndex))
        return text(of: sxml_byte_view(bytes: bytes, byte_count: byteCount))
    }
}

/// The conventional prefixes of the optional features' namespaces, bound
/// for XPath wherever the feature is present, as the sxml command binds
/// them (EXPath Binary 4.0, 1.5; EXPath File 4.0, 1.7; D235).
private let featurePrefixes: [(feature: String, prefix: String, uri: String)] = [
    ("expath-binary", "bin", "http://expath.org/ns/binary"),
    ("expath-file", "file", "http://expath.org/ns/file"),
]

/// What the resolver function needs, apart from the Processor object (the
/// shim's state pointer is set before the processor exists).
final class ProcessorState {
    let resolver: Resolver?
    let collationData: CollationData
    /// The error the resolver threw during the current call.
    var pendingError: (any Error)?
    var isInResolver = false

    init(resolver: Resolver?, collationData: CollationData) {
        self.resolver = resolver
        self.collationData = collationData
    }
}

/// A processor (include/sxml/processor.h): its name pool, types and items.
/// Values and expressions belong to the processor that made them and keep
/// it alive.
public final class Processor {
    let handle: OpaquePointer
    let state: ProcessorState
    /// The namespaces of the processor's features (D235).
    private let featureNamespaces: [(prefix: String, uri: String)]
    /// parse-json($json), compiled when first needed.
    private var jsonExpression: OpaquePointer?
    /// For tests: told the allocations left when the processor is
    /// destroyed (0 unless the package or sxml leaked).
    var leakObserver: ((Int) -> Void)?

    /// Makes a processor.
    /// - Parameters:
    ///   - resolver: answers fn:doc and the other resource functions;
    ///     default: nothing is available.
    ///   - features: the optional features the processor may use, by name,
    ///     each one of `Sxml.features`; nil: every one.
    ///   - collationData: the DUCET for the uca-collation feature.
    ///   - failingAllocationNumber: for tests, fails this allocation
    ///     (counted from 1) with out of memory; 0 fails none.
    public init(resolver: Resolver? = nil, features: Set<String>? = nil,
                collationData: CollationData = .bundled, failingAllocationNumber: Int = 0) throws {
        var featureMask: UInt32 = 0
        let selected = features ?? Set(Sxml.features)
        for name in selected {
            guard let featureIndex = Sxml.features.firstIndex(of: name) else {
                throw SxmlUsageError.unknownFeature(name)
            }
            featureMask |= UInt32(1) << UInt32(featureIndex)
        }
        state = ProcessorState(resolver: resolver, collationData: collationData)
        var configuration = sxml_host_processor_configuration(
            failing_allocation_number: UInt32(clamping: failingAllocationNumber),
            feature_mask: featureMask,
            resolver: sxml_host_resolver(
                state: Unmanaged.passUnretained(state).toOpaque(), resolve: resolveResource),
            source_code_languages: sxml_source_code_language_view(languages: nil, language_count: 0))
        guard let handle = sxml_host_processor_create(&configuration) else {
            throw SxmlError.outOfMemory
        }
        self.handle = handle
        featureNamespaces = featurePrefixes.filter { selected.contains($0.feature) }
            .map { (prefix: $0.prefix, uri: $0.uri) }
    }

    deinit {
        if let jsonExpression {
            sxml_host_expression_destroy(jsonExpression)
        }
        let leakedAllocationCount = sxml_host_processor_destroy(handle)
        leakObserver?(Int(leakedAllocationCount))
    }

    /// Parses an XML document and returns its document node.
    public func parse(_ xml: String, uri: String? = nil) throws -> Value {
        try parseDocument(xml.utf8, uri: uri)
    }

    /// Parses an XML document in any encoding it declares.
    public func parse(_ xml: [UInt8], uri: String? = nil) throws -> Value {
        try parseDocument(xml, uri: uri)
    }

    /// Compiles an XPath 4.0 expression, for evaluating it many times.
    /// - Parameters:
    ///   - variables: the external variables, as NCNames or Q{uri}local,
    ///     each item()*.
    ///   - namespaces: prefix to URI; the prefix "" sets the default element
    ///     namespace. Added to the predeclared prefixes and to those of the
    ///     processor's features, which they may rebind.
    ///   - baseURI: the static base URI, against which fn:doc resolves.
    public func compile(_ expression: String, variables: [String] = [],
                        namespaces: [String: String] = [:], baseURI: String? = nil) throws
        -> XPathExpression
    {
        try enter()
        let compiled = try compileHandle(
            expression, variables: variables, namespaces: namespaces, baseURI: baseURI)
        return XPathExpression(processor: self, handle: compiled, variables: variables)
    }

    /// Compiles and evaluates an expression once; its variables are the
    /// keys of variables.
    public func evaluate(_ expression: String, context: (any XdmInput)? = nil,
                         variables: [String: (any XdmInput)?] = [:],
                         namespaces: [String: String] = [:], baseURI: String? = nil) throws -> Value
    {
        let compiled = try compile(
            expression, variables: Array(variables.keys), namespaces: namespaces, baseURI: baseURI)
        return try compiled.evaluate(context: context, variables: variables)
    }

    /// Converts data to a value (see `XdmInput`): a Value of this processor
    /// as it is, a String as an xs:string, other data as fn:parse-json
    /// reads its JSON text (map keys in code point order).
    public func value(_ data: (any XdmInput)?) throws -> Value {
        if let value = data as? Value {
            try check(value)
            return value
        }
        try enter()
        if let string = data as? String {
            return try stringValue(string)
        }
        let json = try stringValue(try jsonText(of: data))
        if jsonExpression == nil {
            jsonExpression = try compileHandle(
                "parse-json($json)", variables: ["json"], namespaces: [:], baseURI: nil)
        }
        guard let jsonExpression else {
            throw SxmlError.outOfMemory
        }
        return try evaluateHandle(jsonExpression, context: nil, variableValues: [json])
    }

    /// Serializes a value: by the adaptive method without options, else as
    /// fn:serialize with them (method xml unless they name another). Keys
    /// may be camelCase (omitXmlDeclaration); yes/no parameters take Bool.
    public func serialize(_ value: Value, options: [String: (any XdmInput)?]? = nil) throws
        -> String
    {
        try enter()
        try check(value)
        let optionsValue = try serializationOptions(options)
        return try serializeWith(value, options: optionsValue)
    }

    /// The allocations the processor holds, for leak checks in tests.
    public func liveAllocationCount() -> Int {
        Int(sxml_host_live_allocation_count(handle))
    }

    // ------------------------------------------------------- For the package

    /// Checks that the processor may be used now, and clears the
    /// resolver's error of an earlier call.
    func enter() throws {
        if state.isInResolver {
            throw SxmlUsageError.resolverUsesProcessor
        }
        state.pendingError = nil
    }

    /// Throws unless value belongs to this processor.
    func check(_ value: Value) throws {
        if value.processor !== self {
            throw SxmlUsageError.valueOfAnotherProcessor
        }
    }

    /// Wraps a value handle an operation returned, or throws its failure.
    func adopt(_ handle: OpaquePointer?) throws -> Value {
        guard let handle else {
            throw failure()
        }
        return Value(processor: self, handle: handle)
    }

    /// An input as a value of this processor; values it made are appended
    /// to made, which the caller keeps until the call is done.
    func input(_ data: (any XdmInput)?, made: inout [Value]) throws -> Value {
        if let value = data as? Value {
            try check(value)
            return value
        }
        let value = try self.value(data)
        made.append(value)
        return value
    }

    /// The last operation's failure: the resolver's error, or the
    /// processor's error record.
    func failure() -> any Error {
        if let pendingError = state.pendingError {
            state.pendingError = nil
            return pendingError
        }
        let error = sxml_host_error_of(handle).pointee
        var statusName = text(of: error.status_name)
        if statusName.hasPrefix("SXML_STATUS_") {
            statusName.removeFirst("SXML_STATUS_".count)
        }
        return SxmlError(
            status: Status(rawValue: statusName) ?? .unknown,
            codeNamespaceURI: text(of: error.code_namespace_uri),
            codeLocalName: text(of: error.code_local_name),
            codePrefix: text(of: error.code_namespace_prefix),
            description: text(of: error.description),
            sourceName: text(of: error.source_name),
            lineNumber: Int(error.line_number),
            columnNumber: Int(error.column_number))
    }

    /// Compiles; returns the expression's handle.
    func compileHandle(_ expression: String, variables: [String], namespaces: [String: String],
                       baseURI: String?) throws -> OpaquePointer
    {
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        let namespaceBindings = try self.namespaceBindings(namespaces, scratch: &scratch)
        var variableNames: [sxml_host_variable] = []
        for name in variables {
            let (namespaceURI, localName) = splitName(name)
            variableNames.append(
                sxml_host_variable(
                    namespace_uri: try scratch.view(namespaceURI), local_name: try scratch.view(localName)))
        }
        var request = sxml_host_compile_request(
            expression_text: try scratch.view(expression),
            static_base_uri: try scratch.view(optional: baseURI),
            namespaces: try scratch.array(namespaceBindings),
            namespace_count: namespaceBindings.count,
            variables: try scratch.array(variableNames),
            variable_count: variableNames.count)
        guard let compiled = sxml_host_compile(handle, &request) else {
            throw failure()
        }
        return compiled
    }

    /// Evaluates the expression with this handle.
    func evaluateHandle(_ expression: OpaquePointer, context: Value?, variableValues: [Value?])
        throws -> Value
    {
        try enter()
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        let values: [OpaquePointer?] = variableValues.map { $0?.handle }
        var request = sxml_host_evaluate_request(
            context_value: context?.handle,
            variable_values: try scratch.array(values),
            variable_value_count: values.count)
        // The values must live until the shim is done with their handles.
        let result = withExtendedLifetime((context, variableValues)) {
            sxml_host_evaluate(expression, &request)
        }
        return try adopt(result)
    }

    /// The $options map of fn:serialize for options, whose keys may be
    /// camelCase; nil for none.
    func serializationOptions(_ options: [String: (any XdmInput)?]?) throws -> Value? {
        guard let options else {
            return nil
        }
        var parameters: [String: (any XdmInput)?] = [:]
        for (key, parameter) in options {
            parameters[kebabCase(key)] = parameter
        }
        return try value(parameters)
    }

    /// Serializes value with a map of parameters, or by the adaptive method
    /// without.
    func serializeWith(_ value: Value, options: Value?) throws -> String {
        try enter()
        var request = sxml_host_serialize_request(value: value.handle, options: options?.handle)
        let status = withExtendedLifetime((value, options)) {
            sxml_host_serialize(handle, &request)
        }
        if status != SXML_STATUS_OK.rawValue {
            throw failure()
        }
        return output()
    }

    /// The bytes of the processor's last serialization, as text.
    func output() -> String {
        text(of: sxml_host_output_of(handle).pointee)
    }

    /// Whether the last operation failed: the resolver threw, or the
    /// processor's error has a status.
    func hasFailed() -> Bool {
        state.pendingError != nil || sxml_host_error_of(handle).pointee.status != SXML_STATUS_OK.rawValue
    }

    /// The namespaces of a compilation (`staticNamespaces`) as the shim's
    /// bindings, copied into scratch.
    func namespaceBindings(_ namespaces: [String: String], scratch: inout Scratch) throws
        -> [sxml_host_namespace]
    {
        var bindings: [sxml_host_namespace] = []
        for (prefix, uri) in staticNamespaces(namespaces) {
            bindings.append(
                sxml_host_namespace(
                    namespace_prefix: try scratch.view(prefix), namespace_uri: try scratch.view(uri)))
        }
        return bindings
    }

    // ------------------------------------------------------------- Private

    private func parseDocument<Bytes: Collection>(_ bytes: Bytes, uri: String?) throws -> Value
    where Bytes.Element == UInt8 {
        try enter()
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        var request = sxml_host_parse_request(
            input: try scratch.view(bytes), document_uri: try scratch.view(optional: uri))
        return try adopt(sxml_host_parse(handle, &request))
    }

    private func stringValue(_ string: String) throws -> Value {
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        var view = try scratch.view(string)
        return try adopt(sxml_host_string(handle, &view))
    }

    /// The namespaces of a compilation: the features' prefixes, then the
    /// caller's, which may rebind them.
    private func staticNamespaces(_ namespaces: [String: String]) -> [(String, String)] {
        var merged: [(String, String)] = featureNamespaces
            .filter { namespaces[$0.prefix] == nil }
            .map { ($0.prefix, $0.uri) }
        merged.append(contentsOf: namespaces.sorted { $0.key < $1.key }.map { ($0.key, $0.value) })
        return merged
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension Processor: Sendable {}

/// omitXmlDeclaration as omit-xml-declaration.
private func kebabCase(_ key: String) -> String {
    var result = ""
    for character in key {
        if character.isUppercase {
            result += "-" + character.lowercased()
        } else {
            result.append(character)
        }
    }
    return result
}

/// The shim's resolver function (sxml_host_resolver): asks the processor's
/// resolver, and serves the DUCET when it has no answer for it. Keeps an
/// error the resolver throws and stops the call with INPUT_OUTPUT_FAILED;
/// the call then rethrows it (D238).
private func resolveResource(
    _ state: UnsafeMutableRawPointer?, _ request: UnsafePointer<sxml_host_resource_request>?,
    _ resource: UnsafeMutablePointer<sxml_host_resource>?
) -> UInt32 {
    guard let state, let request, let resource else {
        return SXML_STATUS_OK.rawValue
    }
    let processorState = Unmanaged<ProcessorState>.fromOpaque(state).takeUnretainedValue()
    let hostRequest = request.pointee
    let kinds = ResourceKind.allCases
    let kindIndex = Int(hostRequest.resource_kind)
    let resourceRequest = ResourceRequest(
        kind: kindIndex < kinds.count ? kinds[kindIndex] : .xmlDocument,
        uriReference: text(of: hostRequest.uri_reference),
        absoluteURI: text(of: hostRequest.absolute_uri))
    var answer: Resource?
    if let resolver = processorState.resolver {
        processorState.isInResolver = true
        defer { processorState.isInResolver = false }
        do {
            answer = try resolver(resourceRequest)
        } catch {
            processorState.pendingError = error
            return SXML_STATUS_INPUT_OUTPUT_FAILED.rawValue
        }
    }
    let bytes: [UInt8]
    switch answer {
    case .text(let text):
        bytes = Array(text.utf8)
    case .bytes(let content):
        bytes = content
    case nil:
        guard resourceRequest.kind == .collationData, resourceRequest.absoluteURI == ducetURI,
            let collationData = processorState.collationData.servedBytes
        else {
            return SXML_STATUS_OK.rawValue
        }
        bytes = collationData
    }
    guard let memory = sxml_host_allocate(hostRequest.processor, bytes.count) else {
        return SXML_STATUS_OUT_OF_MEMORY.rawValue
    }
    bytes.withUnsafeBytes { source in
        if let baseAddress = source.baseAddress {
            memory.copyMemory(from: baseAddress, byteCount: source.count)
        }
    }
    resource.pointee.resource_form = SXML_RESOURCE_FORM_BYTES.rawValue
    resource.pointee.content = sxml_byte_view(
        bytes: memory.assumingMemoryBound(to: UInt8.self), byte_count: bytes.count)
    return SXML_STATUS_OK.rawValue
}
