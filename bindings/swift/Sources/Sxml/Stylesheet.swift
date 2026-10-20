// XSLT 4.0 (include/sxml/xslt.h; D216, D238). Golden:
// bindings/javascript/src/index.ts (compileStylesheet, Stylesheet).
internal import CSxmlHost

/// How a result reaches you (XSLT 4.0, 2.4.6), by the values of
/// `sxml_xslt_result_delivery`.
public enum ResultDelivery: UInt32, Sendable, CaseIterable {
    /// As the value the stylesheet made.
    case raw = 1
    /// As one document node.
    case tree = 2
    /// As text, by the output definition.
    case serialized = 3
}

/// The initial mode of a transformation (XSLT 4.0, 2.4.3): a mode by
/// NCName or Q{uri}local, or the unnamed mode. A string literal names a
/// mode.
public enum InitialMode: Sendable, Equatable, ExpressibleByStringLiteral {
    case named(String)
    case unnamed

    public init(stringLiteral name: String) {
        self = .named(name)
    }
}

/// A secondary result: what one xsl:result-document made (XSLT 4.0, 26.2).
public struct ResultDocument {
    /// Its href resolved against the base output URI.
    public let uri: String
    /// `.serialized`: the text.
    public let output: String?
    /// `.raw` and `.tree`: the value.
    public let value: Value?
}

/// What a transformation made.
public struct TransformResult {
    /// The principal result serialized (`.serialized`).
    public let output: String?
    /// The principal result as a value (`.raw`, `.tree`).
    public let value: Value?
    /// In the order they completed.
    public let resultDocuments: [ResultDocument]
}

extension Processor {
    /// Compiles an XSLT 4.0 stylesheet, with the modules it includes and
    /// imports (kind `.xsltModule`) and the packages it uses (kind
    /// `.xsltPackage`), for running it many times.
    /// - Parameters:
    ///   - baseURI: the principal module's URI, against which xsl:include,
    ///     xsl:import and fn:doc resolve.
    ///   - staticParameters: static parameters (XSLT 4.0, 9.6), by NCName
    ///     or Q{uri}local.
    public func compileStylesheet(_ stylesheet: String, baseURI: String? = nil,
                                  staticParameters: [String: (any XdmInput)?] = [:]) throws
        -> Stylesheet
    {
        try compileStylesheetText(stylesheet.utf8, baseURI: baseURI, staticParameters: staticParameters)
    }

    /// Compiles a stylesheet in any encoding it declares.
    public func compileStylesheet(_ stylesheet: [UInt8], baseURI: String? = nil,
                                  staticParameters: [String: (any XdmInput)?] = [:]) throws
        -> Stylesheet
    {
        try compileStylesheetText(stylesheet, baseURI: baseURI, staticParameters: staticParameters)
    }

    private func compileStylesheetText<Bytes: Collection>(
        _ bytes: Bytes, baseURI: String?, staticParameters: [String: (any XdmInput)?]
    ) throws -> Stylesheet where Bytes.Element == UInt8 {
        try enter()
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        var made: [Value] = []
        var request = sxml_host_stylesheet_compile_request(
            stylesheet_text: try scratch.view(bytes),
            static_base_uri: try scratch.view(optional: baseURI),
            static_parameters: try bindingList(staticParameters, scratch: &scratch, made: &made))
        let compiled = withExtendedLifetime(made) {
            sxml_host_stylesheet_compile(handle, &request)
        }
        guard let compiled else {
            throw failure()
        }
        return Stylesheet(processor: self, handle: compiled)
    }
}

/// A compiled XSLT 4.0 stylesheet (include/sxml/xslt.h). It keeps its
/// processor alive.
public final class Stylesheet {
    public let processor: Processor
    private let handle: OpaquePointer

    init(processor: Processor, handle: OpaquePointer) {
        self.processor = processor
        self.handle = handle
    }

    deinit {
        sxml_host_stylesheet_destroy(handle)
    }

    /// Runs one transformation (XSLT 4.0, 2.3): applies templates to the
    /// source, or calls initialTemplate or initialFunction.
    /// - Parameters:
    ///   - source: the global context item (2.4.2): its document is the
    ///     default initial match selection.
    ///   - initialMatchSelection: the initial match selection, when it is
    ///     not the source.
    ///   - initialMode: default: the stylesheet's default mode.
    ///   - initialTemplate: calls this named template instead of applying
    ///     templates (2.4.4). Without source and initialMatchSelection,
    ///     xsl:initial-template is called.
    ///   - initialFunction: calls this public stylesheet function with
    ///     arguments instead (2.4.5).
    ///   - parameters: the stylesheet parameters.
    ///   - templateParameters, tunnelParameters: parameters of the initial
    ///     template or of the template rules.
    ///   - baseOutputURI: against which relative hrefs of
    ///     xsl:result-document resolve.
    ///   - delivery: the principal result's; `.serialized` by the
    ///     stylesheet's unnamed xsl:output.
    ///   - resultDocumentDelivery: the result documents'.
    ///   - serialization: serialization parameters replacing the unnamed
    ///     xsl:output's, as fn:serialize takes them (keys may be camelCase).
    ///   - messages: receives xsl:message and xsl:assert output after the
    ///     transformation, also when it throws.
    public func transform(
        source: (any XdmInput)? = nil,
        initialMatchSelection: (any XdmInput)? = nil,
        initialMode: InitialMode? = nil,
        initialTemplate: String? = nil,
        initialFunction: String? = nil,
        arguments: [(any XdmInput)?] = [],
        parameters: [String: (any XdmInput)?] = [:],
        templateParameters: [String: (any XdmInput)?] = [:],
        tunnelParameters: [String: (any XdmInput)?] = [:],
        baseOutputURI: String? = nil,
        delivery: ResultDelivery = .serialized,
        resultDocumentDelivery: ResultDelivery = .serialized,
        serialization: [String: (any XdmInput)?]? = nil,
        messages: ((Message) throws -> Void)? = nil
    ) throws -> TransformResult {
        try processor.enter()
        var scratch = Scratch(processor: processor.handle)
        defer { scratch.release() }
        var made: [Value] = []
        let sourceValue = try source.map { try processor.input($0, made: &made) }
        let selection = try initialMatchSelection.map { try processor.input($0, made: &made) }
        var argumentValues: [Value] = []
        for argument in arguments {
            argumentValues.append(try processor.input(argument, made: &made))
        }
        let serializationValue = try processor.serializationOptions(serialization)
        // XSLT 4.0, 2.3: a function call, a named template (also
        // xsl:initial-template without anything to apply templates to), or
        // apply-templates.
        let invocation: InvocationKind
        if initialFunction != nil {
            invocation = .callFunction
        } else if initialTemplate != nil || (source == nil && initialMatchSelection == nil) {
            invocation = .callTemplate
        } else {
            invocation = .applyTemplates
        }
        var modeName: String?
        if case .named(let name) = initialMode {
            modeName = name
        }
        let argumentHandles: [OpaquePointer?] = argumentValues.map { $0.handle }
        var request = sxml_host_transform_request(
            global_context_item: sourceValue?.handle,
            initial_match_selection: selection?.handle,
            stylesheet_parameters: try processor.bindingList(parameters, scratch: &scratch, made: &made),
            template_parameters: try processor.bindingList(templateParameters, scratch: &scratch, made: &made),
            tunnel_parameters: try processor.bindingList(tunnelParameters, scratch: &scratch, made: &made),
            function_arguments: try scratch.array(argumentHandles),
            function_argument_count: argumentHandles.count,
            base_output_uri: try scratch.view(optional: baseOutputURI),
            serialization_options: serializationValue?.handle,
            initial_mode: try scratch.name(modeName),
            initial_template: try scratch.name(initialTemplate),
            initial_function: try scratch.name(initialFunction),
            invocation_kind: invocation.rawValue,
            result_delivery: delivery.rawValue,
            result_document_delivery: resultDocumentDelivery.rawValue,
            selects_unnamed_mode: initialMode == .unnamed ? 1 : 0,
            side_results: noSideResults)
        let result = withExtendedLifetime((sourceValue, selection, argumentValues, serializationValue, made)) {
            sxml_host_stylesheet_transform(handle, &request)
        }
        // Read before a callback can use the processor.
        let failure = result == nil ? processor.failure() : nil
        let output = result != nil && delivery == .serialized ? processor.output() : nil
        let sideResults = processor.sideResults(request.side_results)
        let value = result.map { Value(processor: processor, handle: $0) }
        try processor.deliverMessages(sideResults, to: messages)
        guard let value else {
            throw failure ?? SxmlError.outOfMemory
        }
        let resultDocuments = sideResults.filter { $0.kind == SideResult.resultDocument }.map {
            ResultDocument(uri: $0.uri, output: $0.first == nil ? $0.text : nil, value: $0.first)
        }
        return TransformResult(
            output: output, value: delivery == .serialized ? nil : value, resultDocuments: resultDocuments)
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension Stylesheet: Sendable {}

/// The values of sxml_xslt_invocation_kind (include/sxml/xslt.h, which
/// sxml_host.h does not include).
private enum InvocationKind: UInt32 {
    case applyTemplates = 0
    case callTemplate = 1
    case callFunction = 2
}
