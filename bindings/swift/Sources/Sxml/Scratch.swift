// Memory for one call's request (golden: bindings/javascript/src/index.ts,
// Scratch): texts copied into the processor's allocator, so that the
// allocation-failure loop (D23) reaches the package's own allocations too,
// and released together when the call is done.
internal import CSxmlHost

/// Allocations from a processor for one call, released by `release()`,
/// which the caller defers right after making it.
struct Scratch {
    private let processor: OpaquePointer
    private var allocations: [UnsafeMutableRawPointer] = []

    init(processor: OpaquePointer) {
        self.processor = processor
    }

    /// byteCount bytes (at least 1) from the processor's allocator.
    mutating func allocate(_ byteCount: Int) throws -> UnsafeMutableRawPointer {
        guard let memory = sxml_host_allocate(processor, byteCount) else {
            throw SxmlError.outOfMemory
        }
        allocations.append(memory)
        return memory
    }

    /// A byte view of a copy of bytes.
    mutating func view<Bytes: Collection>(_ bytes: Bytes) throws -> sxml_byte_view
    where Bytes.Element == UInt8 {
        let memory = try allocate(bytes.count)
        let buffer = UnsafeMutableRawBufferPointer(start: memory, count: bytes.count)
        _ = buffer.initializeMemory(as: UInt8.self, from: bytes)
        return sxml_byte_view(bytes: memory.assumingMemoryBound(to: UInt8.self), byte_count: bytes.count)
    }

    /// A byte view of a copy of text's UTF-8.
    mutating func view(_ text: String) throws -> sxml_byte_view {
        try view(text.utf8)
    }

    /// A byte view of text, or the empty view for nil.
    mutating func view(optional text: String?) throws -> sxml_byte_view {
        guard let text else {
            return sxml_byte_view(bytes: nil, byte_count: 0)
        }
        return try view(text)
    }

    /// A copy of elements in the processor's memory.
    mutating func array<Element>(_ elements: [Element]) throws -> UnsafePointer<Element>? {
        guard !elements.isEmpty else {
            return nil
        }
        let memory = try allocate(MemoryLayout<Element>.stride * elements.count)
        let typed = memory.bindMemory(to: Element.self, capacity: elements.count)
        typed.initialize(from: elements, count: elements.count)
        return UnsafePointer(typed)
    }

    /// Releases every allocation.
    mutating func release() {
        for memory in allocations {
            sxml_host_release(processor, memory)
        }
        allocations.removeAll()
    }
}

/// The text of a byte view (UTF-8; sxml writes valid UTF-8).
func text(of view: sxml_byte_view) -> String {
    guard let bytes = view.bytes, view.byte_count > 0 else {
        return ""
    }
    return String(decoding: UnsafeBufferPointer(start: bytes, count: view.byte_count), as: UTF8.self)
}

/// Splits an NCName or Q{uri}local into namespace URI and local name.
func splitName(_ name: String) -> (namespaceURI: String, localName: String) {
    guard name.hasPrefix("Q{"), let close = name.firstIndex(of: "}") else {
        return ("", name)
    }
    let uri = name[name.index(name.startIndex, offsetBy: 2)..<close]
    let local = name[name.index(after: close)...]
    if local.isEmpty || uri.contains("{") {
        return ("", name)
    }
    return (String(uri), String(local))
}
