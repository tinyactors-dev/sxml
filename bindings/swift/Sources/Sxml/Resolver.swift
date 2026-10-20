// Resources (include/sxml/processor.h, "Resource resolver"; D238): what
// a resolver is asked for and what it answers. Golden:
// bindings/javascript/src/index.ts, ResourceKind and Resolver.
import Foundation

/// What a resolver is asked for, by the JavaScript package's names, in
/// the order of `sxml_resource_kind`.
public enum ResourceKind: String, Sendable, CaseIterable {
    case xmlDocument = "xml-document"
    case text = "text"
    case binary = "binary"
    case collection = "collection"
    case uriCollection = "uri-collection"
    case xqueryModule = "xquery-module"
    case xsltModule = "xslt-module"
    case xsltPackage = "xslt-package"
    case schemaDocument = "schema-document"
    case externalEntity = "external-entity"
    /// A file of the IANA Time Zone Database for the timezone-database
    /// feature: absoluteURI is "urn:sxml:timezone-data:" and a zoneinfo
    /// path. Answer with the file's bytes.
    case timezoneData = "timezone-data"
    /// The DUCET for the uca-collation feature: absoluteURI is
    /// "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt". Answer with
    /// the file's text, or leave it to the processor's `CollationData`.
    case collationData = "collation-data"
    /// A CLDR file for the localization feature: absoluteURI is
    /// "urn:sxml:locale-data:" and a path below CLDR's common/ directory.
    case localeData = "locale-data"
}

public struct ResourceRequest: Sendable, Equatable {
    public let kind: ResourceKind
    /// The URI as the expression wrote it, such as fn:doc's argument.
    public let uriReference: String
    /// uriReference resolved against the static base URI; "" when that is
    /// absent and uriReference is relative.
    public let absoluteURI: String

    public init(kind: ResourceKind, uriReference: String, absoluteURI: String) {
        self.kind = kind
        self.uriReference = uriReference
        self.absoluteURI = absoluteURI
    }
}

/// A resolver's answer: the resource's text (UTF-8) or bytes.
public enum Resource: Sendable, Equatable {
    case text(String)
    case bytes([UInt8])
}

/// Answers fn:doc, fn:unparsed-text and the other resource functions:
/// the resource, or nil when it is not available (FODC0002 for fn:doc).
/// Called synchronously during a call on the processor; it must not use
/// that processor (`SxmlUsageError.resolverUsesProcessor`), and so must not
/// capture it strongly either (a cycle). An error it throws stops the call,
/// which rethrows that error.
public typealias Resolver = (ResourceRequest) throws -> Resource?

/// The DUCET (allkeys.txt of the Unicode Collation Algorithm 16.0.0) that
/// the uca-collation feature reads (D213), served when the processor's
/// resolver has no answer for `.collationData`: a resolver's own answer
/// comes first.
public enum CollationData: Sendable, Equatable {
    /// The copy the package ships, a SwiftPM resource of module Sxml
    /// (`Bundle.module`), read when a UCA collation is first used.
    case bundled
    /// None: UCA collation URIs use sxml's fallback (fallback=no is
    /// FOCH0002).
    case none
    /// Another copy of allkeys.txt.
    case bytes([UInt8])

    /// The bytes this choice serves.
    var servedBytes: [UInt8]? {
        switch self {
        case .bundled:
            return bundledCollationData
        case .none:
            return nil
        case .bytes(let bytes):
            return bytes
        }
    }
}

/// The package's DUCET, read from its resource bundle once per process,
/// when first asked for; nil when the bundle has no copy.
private let bundledCollationData: [UInt8]? = {
    guard let url = Bundle.module.url(forResource: "allkeys", withExtension: "txt"),
        let data = try? Data(contentsOf: url)
    else {
        return nil
    }
    return [UInt8](data)
}()

/// The URI the uca-collation feature asks for its DUCET by.
let ducetURI = "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt"
