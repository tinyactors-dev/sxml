// XSD 1.1 validation (include/sxml/schema.h; D216, D238). Golden:
// bindings/javascript/src/index.ts (compileSchema, Schema,
// schemaReports, SchemaError).
internal import CSxmlHost

/// A schema document: its text, with its URI, or only a URI, which the
/// resolver is asked for (kind `.schemaDocument`).
public enum SchemaDocument: Sendable, Equatable {
    case text(String, uri: String? = nil)
    case bytes([UInt8], uri: String? = nil)
    case resolved(String)
}

/// One error of a compilation or validation (XSD 1.1 Part 1, appendix C).
public struct SchemaReport: Sendable, Equatable {
    /// The constraint name, such as "cvc-complex-type.2.4".
    public let code: String
    public let description: String
    /// The schema document's URI, or the instance's.
    public let sourceName: String
    public let lineNumber: Int
    public let columnNumber: Int
}

/// A schema that does not compile: every report, and the failure, whose
/// code, description and location are the first report's.
public struct SchemaError: Error, CustomStringConvertible, Sendable {
    public let error: SxmlError
    public let reports: [SchemaReport]

    public var description: String { error.message }
}

/// How the validation root is validated (XSD 1.1 Part 1, 5.2), by the
/// values of `sxml_validation_mode`.
public enum ValidationMode: UInt32, Sendable, CaseIterable {
    /// The root needs a global declaration.
    case strict = 0
    case lax = 1
}

/// The validity of the validation root (Part 1, 3.3.5.1), by the names of
/// the JavaScript package.
public enum Validity: String, Sendable, CaseIterable {
    case valid = "valid"
    case invalid = "invalid"
    case notKnown = "not-known"
}

/// What a validation found.
public struct ValidationResult {
    public let validity: Validity
    /// Every error found, in order; empty when valid.
    public let reports: [SchemaReport]
    /// Unless invalid: the validated copy, its elements and attributes
    /// annotated with their types (XDM 4.0, 3.3).
    public let document: Value?
}

extension Processor {
    /// Compiles an XSD 1.1 schema from one schema document's text.
    public func compileSchema(_ text: String, baseURI: String? = nil) throws -> Schema {
        try compileSchema([.text(text)], baseURI: baseURI)
    }

    /// Compiles an XSD 1.1 schema from one schema document.
    public func compileSchema(_ document: SchemaDocument, baseURI: String? = nil) throws -> Schema {
        try compileSchema([document], baseURI: baseURI)
    }

    /// Compiles an XSD 1.1 schema from schema documents, with every
    /// document they include, import, redefine or override (the resolver
    /// is asked, kind `.schemaDocument`). Throws a `SchemaError` with every
    /// report when the schema has errors.
    /// - Parameter baseURI: against which relative URIs of documents given
    ///   by URI resolve.
    public func compileSchema(_ documents: [SchemaDocument], baseURI: String? = nil) throws -> Schema {
        try enter()
        guard let created = sxml_host_schema_create(handle) else {
            throw SxmlError.outOfMemory
        }
        // Destroyed by ARC when the compilation throws.
        let schema = Schema(processor: self, handle: created)
        var scratch = Scratch(processor: handle)
        defer { scratch.release() }
        var hostDocuments: [sxml_host_schema_document] = []
        for document in documents {
            switch document {
            case .text(let text, let uri):
                hostDocuments.append(
                    sxml_host_schema_document(text: try scratch.view(text), uri: try scratch.view(optional: uri)))
            case .bytes(let bytes, let uri):
                hostDocuments.append(
                    sxml_host_schema_document(text: try scratch.view(bytes), uri: try scratch.view(optional: uri)))
            case .resolved(let uri):
                hostDocuments.append(
                    sxml_host_schema_document(
                        text: sxml_byte_view(bytes: nil, byte_count: 0), uri: try scratch.view(uri)))
            }
        }
        var request = sxml_host_schema_compile_request(
            documents: try scratch.array(hostDocuments),
            document_count: hostDocuments.count,
            base_uri: try scratch.view(optional: baseURI),
            side_results: noSideResults)
        let status = sxml_host_schema_compile(created, &request)
        let reports = schemaReports(sideResults(request.side_results))
        if status != SXML_STATUS_OK.rawValue {
            let failure = failure()
            if let error = failure as? SxmlError {
                throw SchemaError(error: error, reports: reports)
            }
            throw failure
        }
        return schema
    }
}

/// The SCHEMA_REPORT side results as reports.
private func schemaReports(_ results: [SideResult]) -> [SchemaReport] {
    results.filter { $0.kind == SideResult.schemaReport }.map {
        SchemaReport(
            code: $0.code, description: $0.text, sourceName: $0.uri, lineNumber: $0.lineNumber,
            columnNumber: $0.columnNumber)
    }
}

/// A compiled XSD 1.1 schema (include/sxml/schema.h). It keeps its
/// processor alive.
public final class Schema {
    public let processor: Processor
    private let handle: OpaquePointer

    init(processor: Processor, handle: OpaquePointer) {
        self.processor = processor
        self.handle = handle
    }

    deinit {
        sxml_host_schema_destroy(handle)
    }

    /// Validates XML text (XSD 1.1 Part 1, 5.2), parsed and validated in
    /// one pass so that reports have its lines and columns. An invalid
    /// document is not an error: see the result's validity and reports.
    /// Text that is not well-formed throws, as `Processor.parse` does.
    /// - Parameters:
    ///   - mode: `.strict`: the root needs a global declaration.
    ///   - type: validates the root against this type of the schema
    ///     instead, by NCName or Q{uri}local.
    ///   - uri: the document URI of the text, its reports' source name.
    public func validate(_ text: String, mode: ValidationMode = .strict, type: String? = nil,
                         uri: String? = nil) throws -> ValidationResult
    {
        try validateInput(node: nil, text: Array(text.utf8), mode: mode, type: type, uri: uri)
    }

    /// Validates an XML document in any encoding it declares.
    public func validate(_ bytes: [UInt8], mode: ValidationMode = .strict, type: String? = nil,
                         uri: String? = nil) throws -> ValidationResult
    {
        try validateInput(node: nil, text: bytes, mode: mode, type: type, uri: uri)
    }

    /// Validates a document or element node of this processor.
    public func validate(_ node: Value, mode: ValidationMode = .strict, type: String? = nil) throws
        -> ValidationResult
    {
        try processor.check(node)
        return try validateInput(node: node, text: [], mode: mode, type: type, uri: nil)
    }

    private func validateInput(node: Value?, text: [UInt8], mode: ValidationMode, type: String?,
                               uri: String?) throws -> ValidationResult
    {
        try processor.enter()
        var scratch = Scratch(processor: processor.handle)
        defer { scratch.release() }
        var request = sxml_host_validate_request(
            node: node?.handle,
            text: node == nil ? try scratch.view(text) : sxml_byte_view(bytes: nil, byte_count: 0),
            document_uri: try scratch.view(optional: uri),
            validation_mode: mode.rawValue,
            governing_type: try scratch.name(type),
            validity: 0,
            side_results: noSideResults)
        let result = withExtendedLifetime(node) {
            sxml_host_schema_validate(handle, &request)
        }
        let failure = result == nil && processor.hasFailed() ? processor.failure() : nil
        let reports = schemaReports(processor.sideResults(request.side_results))
        let document = result.map { Value(processor: processor, handle: $0) }
        if let failure {
            throw failure
        }
        let validities: [Validity] = [.valid, .invalid, .notKnown]
        let validity = Int(request.validity) < validities.count ? validities[Int(request.validity)] : .invalid
        return ValidationResult(validity: validity, reports: reports, document: document)
    }
}

@available(*, unavailable, message: "a processor is used by one thread at a time (D35)")
extension Schema: Sendable {}
