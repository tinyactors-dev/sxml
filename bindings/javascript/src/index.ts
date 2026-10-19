// sxml for JavaScript: XML parsing, XPath 4.0 and serialization in Node.js
// and browsers, over sxml.wasm (docs/DECISIONS.md, D10, D138).
//
//     import { load } from 'sxml-js';
//     const sxml = await load();
//     const processor = sxml.createProcessor();
//     const book = processor.parse('<book><title>XPath</title></book>');
//     processor.evaluate('//title/string()', { context: book }).toString();
//     // '"XPath"' (the adaptive method of Serialization 4.0)
//
// Every object that owns memory in the module (Processor, XPathExpression,
// Value) has dispose(); one that is never disposed is released when it is
// garbage collected (FinalizationRegistry). The C side is the shim in
// ../sxml_javascript.c; the word offsets below follow its structs.

import { wasiImports } from './wasi.ts';

// ------------------------------------------------------------------ Types

/** Where load() finds sxml.wasm: a URL or path, the bytes, a compiled
 *  module, or a fetch Response. */
export type WasmSource =
  | string
  | URL
  | BufferSource
  | WebAssembly.Module
  | Response
  | PromiseLike<Response>;

export interface LoadOptions {
  /** Default: sxml.wasm next to this file, the core module, which has no
   *  optional feature. sxml-full.wasm, beside it, has every optional
   *  feature (Sxml.features). */
  wasm?: WasmSource;
}

/** What a resolver is asked for (include/sxml/processor.h, "Resource
 *  resolver"). */
export type ResourceKind =
  | 'xml-document'
  | 'text'
  | 'binary'
  | 'collection'
  | 'uri-collection'
  | 'xquery-module'
  | 'xslt-module'
  | 'xslt-package'
  | 'schema-document'
  | 'external-entity'
  /** A file of the IANA Time Zone Database for the full module's
   *  timezone-database feature: absoluteUri is 'urn:sxml:timezone-data:'
   *  and a zoneinfo path, a zone name ('Europe/Paris', a TZif file) or
   *  'zone1970.tab'. Answer with the file's bytes. */
  | 'timezone-data';

export interface ResourceRequest {
  kind: ResourceKind;
  /** The URI as the expression wrote it, such as fn:doc's argument. */
  uriReference: string;
  /** uriReference resolved against the static base URI; '' when that is
   *  absent and uriReference is relative. */
  absoluteUri: string;
}

/** Answers fn:doc, fn:unparsed-text and the other resource functions: the
 *  resource's bytes or text, or null/undefined when it is not available
 *  (FODC0002 for fn:doc). Called synchronously, during an evaluation; it
 *  must not use the processor. An exception it throws stops the evaluation
 *  and is rethrown by the call that started it. */
export type Resolver = (request: ResourceRequest) => string | Uint8Array | null | undefined;

export interface ProcessorOptions {
  /** Default: nothing is available. */
  resolver?: Resolver;
  /** The optional features the processor may use, by name, each one of
   *  Sxml.features. Default: every feature of the module. Without a
   *  feature, sxml behaves as the specifications say a processor without
   *  it does. */
  features?: string[];
  /** For tests: fail this allocation (counted from 1) with out of memory. */
  failingAllocationNumber?: number;
}

/** JSON-like data, converted to XDM as fn:parse-json converts its JSON text
 *  (objects are maps, arrays are arrays, numbers xs:double, null the empty
 *  sequence); a string is an xs:string. */
export type JsonData = string | number | boolean | null | JsonData[] | { [key: string]: JsonData };

/** A value to bind: one this processor made, or JSON-like data. */
export type Input = Value | JsonData;

export interface StaticOptions {
  /** Namespace bindings, prefix to URI; the prefix '' sets the default
   *  element namespace. Added to the predeclared prefixes (xs, fn, map,
   *  array, math, err, ...). */
  namespaces?: Record<string, string>;
  /** The static base URI, against which fn:doc resolves relative URIs. */
  baseUri?: string;
}

export interface CompileOptions extends StaticOptions {
  /** The external variables, as NCNames or Q{uri}local, each item()*. */
  variables?: string[];
}

export interface EvaluateOptions {
  /** The context value; absent means no focus (XPDY0002 when used). */
  context?: Input;
  /** Values of the declared variables; a missing one raises XPDY0002. */
  variables?: Record<string, Input>;
}

/** The serialization parameters of Serialization 4.0, as fn:serialize
 *  takes them: method ('xml', 'html', 'xhtml', 'text', 'json', 'adaptive',
 *  'csv'), indent, omit-xml-declaration, item-separator, ... Keys may be
 *  written in camelCase (omitXmlDeclaration); yes/no parameters take
 *  booleans. Without options, a value is written by the adaptive method. */
export type SerializationOptions = Record<string, JsonData>;

// ----------------------------------------------------------------- Errors

/** A failure in sxml: a static, dynamic or type error of the expression
 *  (with its error code), a document that is not well-formed, a limit, or
 *  out of memory. */
export class SxmlError extends Error {
  /** The status, such as 'DYNAMIC_ERROR' or 'MALFORMED_INPUT'. */
  readonly status: string;
  /** The error code as prefix:local, such as 'err:FOAR0001'; '' when the
   *  status has none. */
  readonly code: string;
  readonly codeNamespaceUri: string;
  readonly codeLocalName: string;
  readonly description: string;
  readonly lineNumber: number;
  readonly columnNumber: number;

  constructor(fields: {
    status: string;
    codeNamespaceUri: string;
    codeLocalName: string;
    codePrefix: string;
    description: string;
    lineNumber: number;
    columnNumber: number;
  }) {
    const code =
      fields.codeLocalName === ''
        ? ''
        : fields.codePrefix !== ''
          ? `${fields.codePrefix}:${fields.codeLocalName}`
          : fields.codeNamespaceUri !== ''
            ? `Q{${fields.codeNamespaceUri}}${fields.codeLocalName}`
            : fields.codeLocalName;
    const where = fields.lineNumber > 0 ? `${fields.lineNumber}:${fields.columnNumber}: ` : '';
    const what = fields.description !== '' ? fields.description : fields.status;
    super(`${where}${code !== '' ? `${code}: ` : ''}${what}`);
    this.name = 'SxmlError';
    this.status = fields.status;
    this.code = code;
    this.codeNamespaceUri = fields.codeNamespaceUri;
    this.codeLocalName = fields.codeLocalName;
    this.description = fields.description;
    this.lineNumber = fields.lineNumber;
    this.columnNumber = fields.columnNumber;
  }
}

// -------------------------------------------------------------- The module

interface Exports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  sxml_javascript_processor_create(failingAllocationNumber: number, featureMask: number): number;
  sxml_javascript_feature_count(): number;
  sxml_javascript_feature_name_bytes(featureIndex: number): number;
  sxml_javascript_feature_name_byte_count(featureIndex: number): number;
  sxml_javascript_processor_destroy(processor: number): number;
  sxml_javascript_allocate(processor: number, byteCount: number): number;
  sxml_javascript_release(processor: number, address: number): void;
  sxml_javascript_live_allocation_count(processor: number): number;
  sxml_javascript_error_of(processor: number): number;
  sxml_javascript_output_of(processor: number): number;
  sxml_javascript_version_text(): number;
  sxml_javascript_parse(processor: number, request: number): number;
  sxml_javascript_value_release(value: number): void;
  sxml_javascript_compile(processor: number, request: number): number;
  sxml_javascript_expression_destroy(expression: number): void;
  sxml_javascript_evaluate(expression: number, request: number): number;
  sxml_javascript_serialize(processor: number, request: number): number;
  sxml_javascript_string(processor: number, text: number): number;
}

// sxml_status values the package itself returns from the resolver import.
const status_ok = 0;
const status_out_of_memory = 6;
const status_input_output_failed = 13;

const resource_kinds: ResourceKind[] = [
  'xml-document',
  'text',
  'binary',
  'collection',
  'uri-collection',
  'xquery-module',
  'xslt-module',
  'xslt-package',
  'schema-document',
  'external-entity',
  'timezone-data',
];

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** What a handle's finalizer needs: kept apart from the JavaScript object,
 *  which is gone by then. */
interface Held {
  processor: ProcessorState;
  kind: 'processor' | 'value' | 'expression';
  address: number;
}

/** A processor's C side and the handles made with it, so that destroying
 *  it releases them first (they could not outlive it in C). */
class ProcessorState {
  readonly module: Sxml;
  address: number;
  readonly resolver: Resolver | undefined;
  /** Handles not yet released: address to kind. */
  readonly live = new Map<number, 'value' | 'expression'>();
  /** The exception the resolver threw during the current call. */
  pendingException: unknown = undefined;
  isInResolver = false;
  /** parse-json($json), compiled when first needed. */
  jsonExpression = 0;
  /** What sxml_javascript_processor_destroy found still allocated. */
  leakedAllocationCount = 0;

  constructor(module: Sxml, address: number, resolver: Resolver | undefined) {
    this.module = module;
    this.address = address;
    this.resolver = resolver;
  }

  releaseHandle(kind: 'value' | 'expression', address: number): void {
    if (this.address === 0 || !this.live.delete(address)) {
      return;
    }
    if (kind === 'value') {
      this.module.exports.sxml_javascript_value_release(address);
    } else {
      this.module.exports.sxml_javascript_expression_destroy(address);
    }
  }

  destroy(): void {
    if (this.address === 0) {
      return;
    }
    for (const [address, kind] of this.live) {
      if (kind === 'value') {
        this.module.exports.sxml_javascript_value_release(address);
      } else {
        this.module.exports.sxml_javascript_expression_destroy(address);
      }
    }
    this.live.clear();
    if (this.jsonExpression !== 0) {
      this.module.exports.sxml_javascript_expression_destroy(this.jsonExpression);
      this.jsonExpression = 0;
    }
    this.module.processors.delete(this.address);
    this.leakedAllocationCount = this.module.exports.sxml_javascript_processor_destroy(
      this.address,
    );
    this.address = 0;
  }
}

/** Memory for one call's request: texts and words, released together. */
class Scratch {
  private readonly state: ProcessorState;
  private readonly addresses: number[] = [];

  constructor(state: ProcessorState) {
    this.state = state;
  }

  allocate(byteCount: number): number {
    const address = this.state.module.exports.sxml_javascript_allocate(
      this.state.address,
      byteCount,
    );
    if (address === 0) {
      throw outOfMemory();
    }
    this.addresses.push(address);
    return address;
  }

  /** Copies text (UTF-8) or bytes in; returns the two words of a byte
   *  view. */
  view(content: string | Uint8Array): [number, number] {
    const bytes = typeof content === 'string' ? encoder.encode(content) : content;
    const address = this.allocate(bytes.byteLength);
    this.state.module.bytes().set(bytes, address);
    return [address, bytes.byteLength];
  }

  /** Writes 32-bit words; returns their address. */
  words(words: number[]): number {
    const address = this.allocate(words.length * 4);
    this.state.module.words().set(words, address / 4);
    return address;
  }

  release(): void {
    for (const address of this.addresses) {
      this.state.module.exports.sxml_javascript_release(this.state.address, address);
    }
    this.addresses.length = 0;
  }
}

function outOfMemory(): SxmlError {
  return new SxmlError({
    status: 'OUT_OF_MEMORY',
    codeNamespaceUri: '',
    codeLocalName: '',
    codePrefix: '',
    description: 'Out of memory',
    lineNumber: 0,
    columnNumber: 0,
  });
}

function disposed(what: string): Error {
  return new Error(`sxml: this ${what} has been disposed`);
}

/** Splits an NCName or Q{uri}local into namespace URI and local name. */
function splitName(name: string): [string, string] {
  const match = /^Q\{([^{}]*)\}(.+)$/.exec(name);
  return match === null ? ['', name] : [match[1] ?? '', match[2] ?? ''];
}

/** The loaded module: makes processors. One per page or process is enough;
 *  processors made from it are independent. */
export class Sxml {
  /** @internal */
  readonly exports: Exports;
  /** The library version, such as '0.2.0'. */
  readonly version: string;
  /** The optional features this module has: none in sxml.wasm; all in
   *  sxml-full.wasm ('test' adds nothing; it checks that the list reaches
   *  the processor). */
  readonly features: readonly string[];
  /** Processors by address, for the resolver import. @internal */
  readonly processors = new Map<number, ProcessorState>();
  private readonly registry: FinalizationRegistry<Held>;

  /** @internal Use load(). */
  constructor(exports: Exports) {
    this.exports = exports;
    this.registry = new FinalizationRegistry((held: Held) => {
      if (held.kind === 'processor') {
        held.processor.destroy();
      } else {
        held.processor.releaseHandle(held.kind, held.address);
      }
    });
    const versionAddress = exports.sxml_javascript_version_text();
    const bytes = this.bytes();
    let end = versionAddress;
    while (bytes[end] !== 0) {
      end += 1;
    }
    this.version = decoder.decode(bytes.subarray(versionAddress, end));
    const features: string[] = [];
    for (let index = 0; index < exports.sxml_javascript_feature_count(); index += 1) {
      features.push(
        this.text(
          exports.sxml_javascript_feature_name_bytes(index),
          exports.sxml_javascript_feature_name_byte_count(index),
        ),
      );
    }
    this.features = Object.freeze(features);
  }

  /** The module's memory as bytes; a new view after every call, since
   *  memory may have grown. @internal */
  bytes(): Uint8Array {
    return new Uint8Array(this.exports.memory.buffer);
  }

  /** @internal */
  words(): Uint32Array {
    return new Uint32Array(this.exports.memory.buffer);
  }

  /** @internal */
  text(address: number, byteCount: number): string {
    return decoder.decode(this.bytes().subarray(address, address + byteCount));
  }

  /** @internal */
  track(owner: object, held: Held): void {
    this.registry.register(owner, held, owner);
  }

  /** @internal */
  untrack(owner: object): void {
    this.registry.unregister(owner);
  }

  createProcessor(options: ProcessorOptions = {}): Processor {
    let featureMask = 0;
    for (const name of options.features ?? this.features) {
      const index = this.features.indexOf(name);
      if (index < 0) {
        throw new Error(`sxml: this module has no optional feature '${name}'`);
      }
      featureMask |= 1 << index;
    }
    const address = this.exports.sxml_javascript_processor_create(
      options.failingAllocationNumber ?? 0,
      featureMask >>> 0,
    );
    if (address === 0) {
      throw outOfMemory();
    }
    const state = new ProcessorState(this, address, options.resolver);
    this.processors.set(address, state);
    const processor = new Processor(state);
    this.track(processor, { processor: state, kind: 'processor', address });
    return processor;
  }

  /** The resolver import (sxml_javascript_host_resolve). @internal */
  resolve(processorAddress: number, requestAddress: number, resourceAddress: number): number {
    const state = this.processors.get(processorAddress);
    if (state === undefined || state.resolver === undefined) {
      return status_ok;
    }
    const request = this.words().subarray(requestAddress / 4, requestAddress / 4 + 5);
    const resourceRequest: ResourceRequest = {
      kind: resource_kinds[request[0] ?? 0] ?? 'xml-document',
      uriReference: this.text(request[1] ?? 0, request[2] ?? 0),
      absoluteUri: this.text(request[3] ?? 0, request[4] ?? 0),
    };
    let answer: string | Uint8Array | null | undefined;
    state.isInResolver = true;
    try {
      answer = state.resolver(resourceRequest);
    } catch (exception) {
      state.pendingException = exception;
      return status_input_output_failed;
    } finally {
      state.isInResolver = false;
    }
    if (answer === null || answer === undefined) {
      return status_ok;
    }
    const bytes = typeof answer === 'string' ? encoder.encode(answer) : answer;
    const address = this.exports.sxml_javascript_allocate(processorAddress, bytes.byteLength);
    if (address === 0) {
      return status_out_of_memory;
    }
    this.bytes().set(bytes, address);
    this.words().set([1, address, bytes.byteLength], resourceAddress / 4);
    return status_ok;
  }
}

/** Loads and instantiates sxml.wasm. */
export async function load(options: LoadOptions = {}): Promise<Sxml> {
  const module = await compileModule(options.wasm ?? new URL('./sxml.wasm', import.meta.url));
  let sxml: Sxml | undefined;
  let memory: WebAssembly.Memory | undefined;
  const instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: wasiImports(module, () => {
      if (memory === undefined) {
        throw new Error('sxml.wasm called WASI before it was instantiated');
      }
      return memory;
    }),
    sxml_host: {
      resolve: (processor: number, request: number, resource: number): number =>
        sxml === undefined ? status_ok : sxml.resolve(processor, request, resource),
    },
  });
  const exports = instance.exports as unknown as Exports;
  memory = exports.memory;
  exports._initialize();
  sxml = new Sxml(exports);
  return sxml;
}

async function compileModule(source: WasmSource): Promise<WebAssembly.Module> {
  if (source instanceof WebAssembly.Module) {
    return source;
  }
  if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
    return WebAssembly.compile(source);
  }
  if (typeof source === 'string' || source instanceof URL) {
    const url = source instanceof URL ? source : new URL(source, import.meta.url);
    if (url.protocol === 'file:') {
      // Node.js: fetch() does not read files.
      const { readFile } = await import('node:fs/promises');
      return WebAssembly.compile(await readFile(url));
    }
    return compileResponse(fetch(url));
  }
  return compileResponse(source);
}

async function compileResponse(response: Response | PromiseLike<Response>): Promise<WebAssembly.Module> {
  const resolved = await response;
  if (!resolved.ok) {
    throw new Error(`sxml: cannot fetch sxml.wasm (${resolved.status})`);
  }
  return WebAssembly.compile(await resolved.arrayBuffer());
}

// --------------------------------------------------------------- Processor

/** A processor (include/sxml/processor.h): its name pool, types and items.
 *  Values and expressions belong to the processor that made them. */
export class Processor {
  private readonly state: ProcessorState;

  /** @internal Use Sxml.createProcessor(). */
  constructor(state: ProcessorState) {
    this.state = state;
  }

  /** Parses an XML document and returns its document node. */
  parse(xml: string | Uint8Array, options: { uri?: string } = {}): Value {
    const state = this.enter();
    const scratch = new Scratch(state);
    try {
      const request = scratch.words([...scratch.view(xml), ...scratch.view(options.uri ?? '')]);
      return this.adopt(state.module.exports.sxml_javascript_parse(state.address, request));
    } finally {
      scratch.release();
    }
  }

  /** Compiles an XPath 4.0 expression, for evaluating it many times. */
  compile(expression: string, options: CompileOptions = {}): XPathExpression {
    const state = this.enter();
    const variables = options.variables ?? [];
    const address = this.compileAddress(expression, options, variables);
    state.live.set(address, 'expression');
    const compiled = new XPathExpression(this, address, variables);
    state.module.track(compiled, { processor: state, kind: 'expression', address });
    return compiled;
  }

  /** Compiles and evaluates an expression once; the variables are those
   *  options.variables names. */
  evaluate(expression: string, options: StaticOptions & EvaluateOptions = {}): Value {
    const compiled = this.compile(expression, {
      ...options,
      variables: Object.keys(options.variables ?? {}),
    });
    try {
      return compiled.evaluate(options);
    } finally {
      compiled.dispose();
    }
  }

  /** Converts JSON-like data to a value (see JsonData). */
  value(data: Input): Value {
    if (data instanceof Value) {
      return data;
    }
    const state = this.enter();
    const scratch = new Scratch(state);
    let text: Value;
    try {
      const view = scratch.words(scratch.view(typeof data === 'string' ? data : JSON.stringify(data)));
      text = this.adopt(state.module.exports.sxml_javascript_string(state.address, view));
    } finally {
      scratch.release();
    }
    if (typeof data === 'string') {
      return text;
    }
    try {
      if (state.jsonExpression === 0) {
        state.jsonExpression = this.compileAddress('parse-json($json)', {}, ['json']);
      }
      return this.evaluateAddress(state.jsonExpression, { variableValues: [text] });
    } finally {
      text.dispose();
    }
  }

  /** Serializes a value: by the adaptive method without options, else as
   *  fn:serialize with them (method xml unless they name another). */
  serialize(value: Value, options?: SerializationOptions): string {
    const state = this.enter();
    this.check(value);
    let optionsValue: Value | undefined;
    if (options !== undefined) {
      const parameters: Record<string, JsonData> = {};
      for (const [key, parameter] of Object.entries(options)) {
        parameters[key.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)] = parameter;
      }
      optionsValue = this.value(parameters);
    }
    const scratch = new Scratch(state);
    try {
      const request = scratch.words([value.address(), optionsValue?.address() ?? 0]);
      if (state.module.exports.sxml_javascript_serialize(state.address, request) !== status_ok) {
        throw this.failure();
      }
      const output = state.module.exports.sxml_javascript_output_of(state.address) / 4;
      const words = state.module.words();
      return state.module.text(words[output] ?? 0, words[output + 1] ?? 0);
    } finally {
      scratch.release();
      optionsValue?.dispose();
    }
  }

  /** Releases the processor and every value and expression made with it. */
  dispose(): void {
    this.state.module.untrack(this);
    this.state.destroy();
  }

  /** The allocations the processor holds, for leak checks in tests; after
   *  dispose(), those that were left when it was destroyed (0 unless the
   *  package or sxml leaked). */
  liveAllocationCount(): number {
    if (this.state.address === 0) {
      return this.state.leakedAllocationCount;
    }
    return this.state.module.exports.sxml_javascript_live_allocation_count(this.state.address);
  }

  // ----------------------------------------------------- For the package

  /** @internal The state, checked usable. */
  enter(): ProcessorState {
    if (this.state.address === 0) {
      throw disposed('processor');
    }
    if (this.state.isInResolver) {
      throw new Error('sxml: a resolver must not use its processor');
    }
    this.state.pendingException = undefined;
    return this.state;
  }

  /** @internal Throws unless value belongs to this processor. */
  check(value: Value): void {
    if (value.processor !== this) {
      throw new Error('sxml: the value belongs to another processor');
    }
  }

  /** @internal Wraps the address an operation returned, or throws its
   *  failure. */
  adopt(address: number): Value {
    if (address === 0) {
      throw this.failure();
    }
    this.state.live.set(address, 'value');
    const value = new Value(this, address);
    this.state.module.track(value, { processor: this.state, kind: 'value', address });
    return value;
  }

  /** @internal Releases a value or expression made with this processor. */
  releaseHandle(owner: object, kind: 'value' | 'expression', address: number): void {
    this.state.module.untrack(owner);
    this.state.releaseHandle(kind, address);
  }

  /** @internal Compiles; returns the expression's address. */
  compileAddress(expression: string, options: StaticOptions, variables: string[]): number {
    const state = this.state;
    const scratch = new Scratch(state);
    try {
      const namespaceWords: number[] = [];
      for (const [prefix, uri] of Object.entries(options.namespaces ?? {})) {
        namespaceWords.push(...scratch.view(prefix), ...scratch.view(uri));
      }
      const variableWords: number[] = [];
      for (const name of variables) {
        const [namespaceUri, localName] = splitName(name);
        variableWords.push(...scratch.view(namespaceUri), ...scratch.view(localName));
      }
      const request = scratch.words([
        ...scratch.view(expression),
        ...scratch.view(options.baseUri ?? ''),
        namespaceWords.length > 0 ? scratch.words(namespaceWords) : 0,
        namespaceWords.length / 4,
        variableWords.length > 0 ? scratch.words(variableWords) : 0,
        variables.length,
      ]);
      const address = state.module.exports.sxml_javascript_compile(state.address, request);
      if (address === 0) {
        throw this.failure();
      }
      return address;
    } finally {
      scratch.release();
    }
  }

  /** @internal Evaluates the expression at address. */
  evaluateAddress(
    expression: number,
    request: { context?: Value; variableValues: (Value | undefined)[] },
  ): Value {
    const state = this.enter();
    const scratch = new Scratch(state);
    try {
      const values = request.variableValues.map((value) => value?.address() ?? 0);
      const words = scratch.words([
        request.context?.address() ?? 0,
        values.length > 0 ? scratch.words(values) : 0,
        values.length,
      ]);
      return this.adopt(state.module.exports.sxml_javascript_evaluate(expression, words));
    } finally {
      scratch.release();
    }
  }

  /** @internal The last operation's failure, or the resolver's exception. */
  failure(): unknown {
    const state = this.state;
    if (state.pendingException !== undefined) {
      const exception = state.pendingException;
      state.pendingException = undefined;
      return exception;
    }
    const module = state.module;
    const at = module.exports.sxml_javascript_error_of(state.address) / 4;
    const words = module.words().slice(at, at + 15);
    const word = (index: number): number => words[index] ?? 0;
    const text = (index: number): string => module.text(word(index), word(index + 1));
    return new SxmlError({
      status: text(1).replace(/^SXML_STATUS_/, ''),
      codeNamespaceUri: text(3),
      codeLocalName: text(5),
      codePrefix: text(7),
      description: text(9),
      lineNumber: word(13),
      columnNumber: word(14),
    });
  }
}

// ------------------------------------------------------------ Expressions

/** A compiled XPath 4.0 expression (include/sxml/xpath.h). */
export class XPathExpression {
  readonly processor: Processor;
  /** The external variables, in the order compile() was given them. */
  readonly variables: readonly string[];
  private handle: number;

  /** @internal Use Processor.compile(). */
  constructor(processor: Processor, address: number, variables: string[]) {
    this.processor = processor;
    this.handle = address;
    this.variables = [...variables];
  }

  /** Evaluates the expression; the result is a new value. */
  evaluate(options: EvaluateOptions = {}): Value {
    if (this.handle === 0) {
      throw disposed('expression');
    }
    const made: Value[] = [];
    const convert = (input: Input | undefined): Value | undefined => {
      if (input === undefined) {
        return undefined;
      }
      if (input instanceof Value) {
        this.processor.check(input);
        return input;
      }
      const value = this.processor.value(input);
      made.push(value);
      return value;
    };
    try {
      const bindings = options.variables ?? {};
      return this.processor.evaluateAddress(this.handle, {
        context: convert(options.context),
        variableValues: this.variables.map((name) =>
          Object.hasOwn(bindings, name) ? convert(bindings[name]) : undefined,
        ),
      });
    } finally {
      for (const value of made) {
        value.dispose();
      }
    }
  }

  dispose(): void {
    if (this.handle !== 0) {
      this.processor.releaseHandle(this, 'expression', this.handle);
      this.handle = 0;
    }
  }
}

// ----------------------------------------------------------------- Values

/** An XDM value: a sequence of items in its processor (a parsed document,
 *  an evaluation's result). */
export class Value {
  readonly processor: Processor;
  private handle: number;

  /** @internal */
  constructor(processor: Processor, address: number) {
    this.processor = processor;
    this.handle = address;
  }

  /** @internal The handle, checked usable. */
  address(): number {
    if (this.handle === 0) {
      throw disposed('value');
    }
    return this.handle;
  }

  /** Serializes the value (Processor.serialize). */
  serialize(options?: SerializationOptions): string {
    return this.processor.serialize(this, options);
  }

  /** The value by the adaptive method: items one per line, strings
   *  quoted, nodes as XML. */
  toString(): string {
    return this.processor.serialize(this);
  }

  dispose(): void {
    if (this.handle !== 0) {
      this.processor.releaseHandle(this, 'value', this.handle);
      this.handle = 0;
    }
  }
}
