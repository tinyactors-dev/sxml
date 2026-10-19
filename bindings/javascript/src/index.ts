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
// sxml-full.wasm adds XQuery 4.0 (Processor.compileQuery), XSLT 4.0
// (Processor.compileStylesheet) and XSD 1.1 validation
// (Processor.compileSchema).
//
// Every object that owns memory in the module (Processor, XPathExpression,
// XQuery, Stylesheet, Schema, Value) has dispose(); one that is never disposed is released when it is
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
  /** The DUCET for a module with the uca-collation feature (D213), the
   *  text of allkeys.txt of the Unicode Collation Algorithm 16.0.0: a URL
   *  or path, or the bytes. Default: allkeys.txt next to this file, which
   *  the package ships; when that cannot be read, UCA collation URIs use
   *  sxml's fallback (and fallback=no is FOCH0002). false: none. A
   *  resolver's own answer for 'collation-data' comes first. */
  collationData?: string | URL | BufferSource | false;
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
  | 'timezone-data'
  /** The DUCET for the full module's uca-collation feature: absoluteUri
   *  is 'http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt'. Answer
   *  with the file's text, or leave it to LoadOptions.collationData. */
  | 'collation-data'
  /** A CLDR file for the full module's localization feature: absoluteUri
   *  is 'urn:sxml:locale-data:' and a path below CLDR's common/
   *  directory ('main/de.xml', 'rbnf/de.xml',
   *  'supplemental/supplementalData.xml', 'supplemental/ordinals.xml').
   *  Answer with the file's bytes; without them dates are English with
   *  '[Language: en]'. */
  | 'locale-data';

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

/** Trace output (XSLT 4.0, 23.1 and 23.2; F&O 4.0, fn:trace and
 *  fn:message): an xsl:message or failed xsl:assert as the text method
 *  writes it, fn:trace and fn:message by the adaptive method. */
export interface Message {
  text: string;
  /** xsl:message: its error code's local name (XTMM9000 by default);
   *  fn:trace and fn:message: the label. */
  code: string;
  /** xsl:message terminate="yes" and xsl:assert: the call then throws. */
  isTerminating: boolean;
  sourceName: string;
  lineNumber: number;
  columnNumber: number;
}

// ------------------------------------------------- XQuery (sxml-full.wasm)

export interface QueryCompileOptions extends StaticOptions {}

/** A tree an updating query changed (XQuery Update Facility 3.0,
 *  upd:applyUpdates): trees never change in sxml, so the update makes a
 *  new tree; putting it where the original was is the host's. */
export interface UpdatedTree {
  /** The root as the query read it: a document node you bound, or a
   *  parentless node. */
  original: Value;
  /** The root of the new tree with every change made. */
  updated: Value;
}

/** A document fn:put stores (XQUF 3.0, 6.1): writing it at uri is the
 *  host's. */
export interface PutDocument {
  /** fn:put's $uri resolved against the static base URI. */
  uri: string;
  node: Value;
  /** fn:put's $params, or undefined. */
  parameters: Value | undefined;
  /** The node serialized with parameters (method xml without). */
  serialize(): string;
}

/** Receives what an updating query changed, after the evaluation and in
 *  the order of each tree's first update; documents fn:put stores come
 *  last. Without putDocument, fn:put raises FOUP0002. */
export interface UpdateSink {
  updatedTree?(tree: UpdatedTree): void;
  putDocument?(document: PutDocument): void;
}

export interface QueryEvaluateOptions {
  /** The initial context value; absent means none (XPDY0002 when the
   *  query needs it). */
  context?: Input;
  /** The external variables the prolog declares (XQuery 4.0, 5.16), by
   *  NCName or Q{uri}local; names it does not declare are ignored, and a
   *  declared one without a value takes its default. */
  variables?: Record<string, Input>;
  /** For an updating query: receives the changed trees. Without it, its
   *  values are disposed at once. */
  updates?: UpdateSink;
  /** Receives fn:trace and fn:message output, after the evaluation (also
   *  when it throws). */
  messages?: (message: Message) => void;
}

// --------------------------------------------------- XSLT (sxml-full.wasm)

export interface StylesheetCompileOptions {
  /** The principal module's URI, against which xsl:include, xsl:import
   *  and fn:doc resolve. */
  baseUri?: string;
  /** Static parameters (XSLT 4.0, 9.6), by NCName or Q{uri}local. */
  staticParameters?: Record<string, Input>;
}

/** How a result reaches you (XSLT 4.0, 2.4.6): 'serialized' as a string,
 *  'raw' as the value the stylesheet made, 'tree' as one document node. */
export type ResultDelivery = 'serialized' | 'raw' | 'tree';

export interface TransformOptions {
  /** The global context item (XSLT 4.0, 2.4.2): its document is the
   *  default initial match selection. */
  source?: Input;
  /** The initial match selection, when it is not the source. */
  initialMatchSelection?: Input;
  /** The initial mode, by NCName or Q{uri}local; '#unnamed' is the unnamed
   *  mode. Default: the stylesheet's default mode. */
  initialMode?: string;
  /** Calls this named template instead of applying templates (2.4.4).
   *  Without source and initialMatchSelection, xsl:initial-template is
   *  called. */
  initialTemplate?: string;
  /** Calls this stylesheet function with args instead (2.4.5). */
  initialFunction?: string;
  args?: Input[];
  /** The stylesheet parameters. */
  parameters?: Record<string, Input>;
  /** Parameters of the initial template or of the template rules. */
  templateParameters?: Record<string, Input>;
  tunnelParameters?: Record<string, Input>;
  /** Against which relative hrefs of xsl:result-document resolve. */
  baseOutputUri?: string;
  /** The principal result's delivery; default 'serialized', by the
   *  stylesheet's unnamed xsl:output. */
  delivery?: ResultDelivery;
  /** The result documents' delivery; default 'serialized'. */
  resultDocumentDelivery?: ResultDelivery;
  /** Serialization parameters replacing the unnamed xsl:output's, as
   *  fn:serialize takes them. */
  serialization?: SerializationOptions;
  /** Receives xsl:message and xsl:assert output, after the
   *  transformation (also when it throws). */
  messages?: (message: Message) => void;
}

/** A secondary result: what one xsl:result-document made (26.2). */
export interface ResultDocument {
  /** Its href resolved against the base output URI. */
  uri: string;
  /** 'serialized': the text. */
  output: string | undefined;
  /** 'raw' and 'tree': the value. */
  value: Value | undefined;
}

export interface TransformResult {
  /** The principal result serialized ('serialized'). */
  output: string | undefined;
  /** The principal result as a value ('raw', 'tree'). */
  value: Value | undefined;
  /** In the order they completed. */
  resultDocuments: ResultDocument[];
}

// ---------------------------------------------------- XSD (sxml-full.wasm)

/** A schema document: its text, with its URI, or only a URI, which the
 *  resolver is asked for (kind 'schema-document'). */
export interface SchemaDocument {
  text?: string | Uint8Array;
  uri?: string;
}

export interface SchemaCompileOptions {
  /** Against which relative URIs of documents given by URI resolve. */
  baseUri?: string;
}

/** One error of a compilation or validation (XSD 1.1 Part 1, appendix
 *  C): code is the constraint name, such as 'cvc-complex-type.2.4'. */
export interface SchemaReport {
  code: string;
  description: string;
  /** The schema document's URI, or the instance's. */
  sourceName: string;
  lineNumber: number;
  columnNumber: number;
}

export interface ValidateOptions {
  /** 'strict' (default): the root needs a global declaration; 'lax'. */
  mode?: 'strict' | 'lax';
  /** Validate the root against this type of the schema instead, by NCName
   *  or Q{uri}local. */
  type?: string;
  /** The document URI of text input. */
  uri?: string;
}

/** The validity of the validation root (Part 1, 3.3.5.1). */
export type Validity = 'valid' | 'invalid' | 'not-known';

export interface ValidationResult {
  validity: Validity;
  /** Every error found, in order; empty when valid. */
  reports: SchemaReport[];
  /** Unless invalid: the validated copy, its elements and attributes
   *  annotated with their types (XDM 4.0, 3.3). */
  document: Value | undefined;
}

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
  /** The module or document the location is in: 'expression', 'query',
   *  a module's URI; '' when unknown. */
  readonly sourceName: string;
  readonly lineNumber: number;
  readonly columnNumber: number;

  constructor(fields: {
    status: string;
    codeNamespaceUri: string;
    codeLocalName: string;
    codePrefix: string;
    description: string;
    sourceName?: string;
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
    this.sourceName = fields.sourceName ?? '';
    this.lineNumber = fields.lineNumber;
    this.columnNumber = fields.columnNumber;
  }
}

/** A schema that does not compile: every error, with the first as the
 *  SxmlError's own code, description and location. */
export class SchemaError extends SxmlError {
  readonly reports: readonly SchemaReport[];

  constructor(error: SxmlError, reports: SchemaReport[]) {
    const prefix = /^([^:{}]+):/.exec(error.code)?.[1] ?? '';
    super({ ...error, codePrefix: prefix });
    this.name = 'SchemaError';
    this.reports = Object.freeze(reports);
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
  // The full module only (sxml_javascript.h, "Full module").
  sxml_javascript_query_compile?(processor: number, request: number): number;
  sxml_javascript_query_destroy?(query: number): void;
  sxml_javascript_query_is_updating?(query: number): number;
  sxml_javascript_query_evaluate?(query: number, request: number): number;
  sxml_javascript_query_serialize?(query: number, value: number): number;
  sxml_javascript_stylesheet_compile?(processor: number, request: number): number;
  sxml_javascript_stylesheet_destroy?(stylesheet: number): void;
  sxml_javascript_stylesheet_transform?(stylesheet: number, request: number): number;
  sxml_javascript_schema_create?(processor: number): number;
  sxml_javascript_schema_destroy?(schema: number): void;
  sxml_javascript_schema_compile?(schema: number, request: number): number;
  sxml_javascript_schema_validate?(schema: number, request: number): number;
}

/** The exports only the full module has, checked present. */
type FullExports = Required<Exports>;

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
  'collation-data',
  'locale-data',
];

/** The URI the uca-collation feature asks for its DUCET by. */
const ducet_uri = 'http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** The kinds of handle a processor makes. */
type HandleKind = 'value' | 'expression' | 'query' | 'stylesheet' | 'schema';

/** What a handle's finalizer needs: kept apart from the JavaScript object,
 *  which is gone by then. */
interface Held {
  processor: ProcessorState;
  kind: 'processor' | HandleKind;
  address: number;
}

/** A processor's C side and the handles made with it, so that destroying
 *  it releases them first (they could not outlive it in C). */
class ProcessorState {
  readonly module: Sxml;
  address: number;
  readonly resolver: Resolver | undefined;
  /** Handles not yet released: address to kind. */
  readonly live = new Map<number, HandleKind>();
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

  releaseHandle(kind: HandleKind, address: number): void {
    if (this.address === 0 || !this.live.delete(address)) {
      return;
    }
    this.destroyHandle(kind, address);
  }

  /** Releases a handle by its kind's export. */
  private destroyHandle(kind: HandleKind, address: number): void {
    const exports = this.module.exports;
    switch (kind) {
      case 'value':
        exports.sxml_javascript_value_release(address);
        break;
      case 'expression':
        exports.sxml_javascript_expression_destroy(address);
        break;
      case 'query':
        exports.sxml_javascript_query_destroy?.(address);
        break;
      case 'stylesheet':
        exports.sxml_javascript_stylesheet_destroy?.(address);
        break;
      case 'schema':
        exports.sxml_javascript_schema_destroy?.(address);
        break;
    }
  }

  destroy(): void {
    if (this.address === 0) {
      return;
    }
    for (const [address, kind] of this.live) {
      this.destroyHandle(kind, address);
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

/** sxml_javascript_side_result_kind. */
const side_result_updated_tree = 0;
const side_result_put_document = 1;
const side_result_result_document = 2;
const side_result_message = 3;
const side_result_schema_report = 4;

/** sxml_xslt_invocation_kind and sxml_xslt_result_delivery. */
const invocation_apply_templates = 0;
const invocation_call_template = 1;
const invocation_call_function = 2;
const result_deliveries: Record<ResultDelivery, number> = { raw: 1, tree: 2, serialized: 3 };

/** sxml_validity. */
const validities: Validity[] = ['valid', 'invalid', 'not-known'];

/** A side result as read from the module (sxml_javascript_side_result). */
interface SideResult {
  kind: number;
  uri: string;
  text: string;
  code: string;
  first: Value | undefined;
  second: Value | undefined;
  lineNumber: number;
  columnNumber: number;
  isTerminating: boolean;
}

/** The four words of a name (sxml_javascript_name); '' gives none. */
function nameWords(scratch: Scratch, name: string | undefined): number[] {
  if (name === undefined || name === '') {
    return [0, 0, 0, 0];
  }
  const [namespaceUri, localName] = splitName(name);
  return [...scratch.view(namespaceUri), ...scratch.view(localName)];
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
  /** The DUCET served for 'collation-data' (LoadOptions.collationData).
   *  @internal */
  collationData: Uint8Array | undefined;
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
    if (state === undefined) {
      return status_ok;
    }
    const request = this.words().subarray(requestAddress / 4, requestAddress / 4 + 5);
    const resourceRequest: ResourceRequest = {
      kind: resource_kinds[request[0] ?? 0] ?? 'xml-document',
      uriReference: this.text(request[1] ?? 0, request[2] ?? 0),
      absoluteUri: this.text(request[3] ?? 0, request[4] ?? 0),
    };
    let answer: string | Uint8Array | null | undefined;
    if (state.resolver !== undefined) {
      state.isInResolver = true;
      try {
        answer = state.resolver(resourceRequest);
      } catch (exception) {
        state.pendingException = exception;
        return status_input_output_failed;
      } finally {
        state.isInResolver = false;
      }
    }
    if (
      (answer === null || answer === undefined) &&
      resourceRequest.kind === 'collation-data' &&
      resourceRequest.absoluteUri === ducet_uri
    ) {
      answer = this.collationData;
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
  if (sxml.features.includes('uca-collation') && options.collationData !== false) {
    if (options.collationData === undefined) {
      // The package's copy; without it the feature falls back (D213).
      sxml.collationData = await readBytes(new URL('./allkeys.txt', import.meta.url)).catch(
        () => undefined,
      );
    } else {
      sxml.collationData = await readBytes(options.collationData);
    }
  }
  return sxml;
}

/** The bytes of a URL, a path or a buffer. */
async function readBytes(source: string | URL | BufferSource): Promise<Uint8Array> {
  if (source instanceof ArrayBuffer) {
    return new Uint8Array(source);
  }
  if (ArrayBuffer.isView(source)) {
    return new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
  }
  const url = source instanceof URL ? source : new URL(source, import.meta.url);
  if (url.protocol === 'file:') {
    const { readFile } = await import('node:fs/promises');
    return new Uint8Array(await readFile(url));
  }
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`sxml: cannot fetch ${url.href} (${response.status})`);
  }
  return new Uint8Array(await response.arrayBuffer());
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

  /** Compiles an XQuery 4.0 main module, with the library modules it
   *  imports (the resolver is asked for them, kind 'xquery-module'), for
   *  evaluating it many times. sxml-full.wasm only. */
  compileQuery(query: string, options: QueryCompileOptions = {}): XQuery {
    const exports = this.full('XQuery');
    const state = this.enter();
    const scratch = new Scratch(state);
    let address: number;
    try {
      const namespaceWords: number[] = [];
      for (const [prefix, uri] of Object.entries(options.namespaces ?? {})) {
        namespaceWords.push(...scratch.view(prefix), ...scratch.view(uri));
      }
      const request = scratch.words([
        ...scratch.view(query),
        ...scratch.view(options.baseUri ?? ''),
        namespaceWords.length > 0 ? scratch.words(namespaceWords) : 0,
        namespaceWords.length / 4,
      ]);
      address = exports.sxml_javascript_query_compile(state.address, request);
      if (address === 0) {
        throw this.failure();
      }
    } finally {
      scratch.release();
    }
    const compiled = new XQuery(this, address, exports.sxml_javascript_query_is_updating(address) !== 0);
    this.track(compiled, 'query', address);
    return compiled;
  }

  /** Compiles an XSLT 4.0 stylesheet, with the modules it includes and
   *  imports (kind 'xslt-module') and the packages it uses (kind
   *  'xslt-package'), for running it many times. sxml-full.wasm only. */
  compileStylesheet(stylesheet: string | Uint8Array, options: StylesheetCompileOptions = {}): Stylesheet {
    const exports = this.full('XSLT');
    const state = this.enter();
    const scratch = new Scratch(state);
    const made: Value[] = [];
    let address: number;
    try {
      const request = scratch.words([
        ...scratch.view(stylesheet),
        ...scratch.view(options.baseUri ?? ''),
        ...this.bindingWords(scratch, options.staticParameters, made),
      ]);
      address = exports.sxml_javascript_stylesheet_compile(state.address, request);
      if (address === 0) {
        throw this.failure();
      }
    } finally {
      scratch.release();
      for (const value of made) {
        value.dispose();
      }
    }
    const compiled = new Stylesheet(this, address);
    this.track(compiled, 'stylesheet', address);
    return compiled;
  }

  /** Compiles an XSD 1.1 schema from one or more schema documents (a
   *  string is a document's text), with every document they include,
   *  import, redefine or override (the resolver is asked, kind
   *  'schema-document'). Throws a SchemaError with every report when the
   *  schema has errors. sxml-full.wasm only. */
  compileSchema(
    documents: string | SchemaDocument | (string | SchemaDocument)[],
    options: SchemaCompileOptions = {},
  ): Schema {
    const exports = this.full('XSD validation');
    const state = this.enter();
    const address = exports.sxml_javascript_schema_create(state.address);
    if (address === 0) {
      throw outOfMemory();
    }
    const schema = new Schema(this, address);
    this.track(schema, 'schema', address);
    const scratch = new Scratch(state);
    try {
      const documentWords: number[] = [];
      const list = Array.isArray(documents) ? documents : [documents];
      for (const document of list) {
        const { text, uri } = typeof document === 'string' ? { text: document, uri: '' } : document;
        documentWords.push(...(text === undefined ? [0, 0] : scratch.view(text)), ...scratch.view(uri ?? ''));
      }
      const request = scratch.words([
        documentWords.length > 0 ? scratch.words(documentWords) : 0,
        list.length,
        ...scratch.view(options.baseUri ?? ''),
        0,
        0,
      ]);
      const status = exports.sxml_javascript_schema_compile(address, request);
      const reports = schemaReports(this.sideResults(request / 4 + 4));
      if (status !== status_ok) {
        const failure = this.failure();
        throw failure instanceof SxmlError ? new SchemaError(failure, reports) : failure;
      }
    } catch (exception) {
      schema.dispose();
      throw exception;
    } finally {
      scratch.release();
    }
    return schema;
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
    this.enter();
    this.check(value);
    const optionsValue = this.serializationOptions(options);
    try {
      return this.serializeWith(value, optionsValue);
    } finally {
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
  releaseHandle(owner: object, kind: HandleKind, address: number): void {
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

  /** @internal options as the map fn:serialize takes, keys in kebab
   *  case; undefined for none. */
  serializationOptions(options: SerializationOptions | undefined): Value | undefined {
    if (options === undefined) {
      return undefined;
    }
    const parameters: Record<string, JsonData> = {};
    for (const [key, parameter] of Object.entries(options)) {
      parameters[key.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)] = parameter;
    }
    return this.value(parameters);
  }

  /** @internal Serializes value with a map or element of parameters, or
   *  by the adaptive method without. */
  serializeWith(value: Value, options: Value | undefined): string {
    const state = this.enter();
    const scratch = new Scratch(state);
    try {
      const request = scratch.words([value.address(), options?.address() ?? 0]);
      if (state.module.exports.sxml_javascript_serialize(state.address, request) !== status_ok) {
        throw this.failure();
      }
      return this.output();
    } finally {
      scratch.release();
    }
  }

  /** @internal The text of the processor's last serialization. */
  output(): string {
    const module = this.state.module;
    const output = module.exports.sxml_javascript_output_of(this.state.address) / 4;
    const words = module.words();
    return module.text(words[output] ?? 0, words[output + 1] ?? 0);
  }

  /** @internal An input as a value of this processor; values it made are
   *  pushed to made, for the caller to dispose. */
  input(data: Input, made: Value[]): Value {
    if (data instanceof Value) {
      this.check(data);
      return data;
    }
    const value = this.value(data);
    made.push(value);
    return value;
  }

  /** @internal The words of bindings (sxml_javascript_binding: name,
   *  value), as a binding list's two words. */
  bindingWords(scratch: Scratch, bindings: Record<string, Input> | undefined, made: Value[]): number[] {
    const words: number[] = [];
    const entries = Object.entries(bindings ?? {});
    for (const [name, data] of entries) {
      words.push(...nameWords(scratch, name), this.input(data, made).address());
    }
    return [words.length > 0 ? scratch.words(words) : 0, entries.length];
  }

  /** @internal The module's full-module exports, or an error saying that
   *  what needs them needs sxml-full.wasm. */
  full(what: string): FullExports {
    const exports = this.state.module.exports;
    if (exports.sxml_javascript_query_compile === undefined) {
      throw new Error(`sxml: ${what} needs the full module, sxml-full.wasm`);
    }
    return exports as FullExports;
  }

  /** @internal Registers the handle of a compiled object. */
  track(owner: object, kind: HandleKind, address: number): void {
    this.state.live.set(address, kind);
    this.state.module.track(owner, { processor: this.state, kind, address });
  }

  /** @internal Adopts a value handle that may be 0 (none). */
  adoptOptional(address: number): Value | undefined {
    return address === 0 ? undefined : this.adopt(address);
  }

  /** @internal The side results a call wrote at words[at], [at + 1]
   *  (sxml_javascript_side_result_list), every value adopted. */
  sideResults(at: number): SideResult[] {
    const module = this.state.module;
    const list = module.words();
    const first = (list[at] ?? 0) / 4;
    const count = list[at + 1] ?? 0;
    const words = module.words().slice(first, first + count * 12);
    const results: SideResult[] = [];
    for (let index = 0; index < count; index += 1) {
      const word = (offset: number): number => words[index * 12 + offset] ?? 0;
      const text = (offset: number): string => module.text(word(offset), word(offset + 1));
      results.push({
        kind: word(0),
        uri: text(1),
        text: text(3),
        code: text(5),
        // Every value is the package's now (sxml_javascript.h).
        first: this.adoptOptional(word(7)),
        second: this.adoptOptional(word(8)),
        lineNumber: word(9),
        columnNumber: word(10),
        isTerminating: word(11) !== 0,
      });
    }
    return results;
  }

  /** @internal Hands the MESSAGE side results to a callback. */
  deliverMessages(results: SideResult[], receive: ((message: Message) => void) | undefined): void {
    for (const result of results) {
      if (result.kind === side_result_message && receive !== undefined) {
        receive({
          text: result.text,
          code: result.code,
          isTerminating: result.isTerminating,
          sourceName: result.uri,
          lineNumber: result.lineNumber,
          columnNumber: result.columnNumber,
        });
      }
    }
  }

  /** @internal Whether the last operation failed: its error has a
   *  status, or the resolver threw. */
  hasFailed(): boolean {
    const state = this.state;
    const at = state.module.exports.sxml_javascript_error_of(state.address) / 4;
    return state.pendingException !== undefined || (state.module.words()[at] ?? 0) !== 0;
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
      sourceName: text(11),
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

// ---------------------------------------------------------------- XQuery

/** A compiled XQuery 4.0 main module (include/sxml/xquery.h).
 *  Processor.compileQuery makes it. */
export class XQuery {
  readonly processor: Processor;
  /** Whether its body is an updating expression (XQUF 3.0, 2.2): its
   *  result is then empty and its changes go to options.updates. */
  readonly isUpdating: boolean;
  private handle: number;

  /** @internal Use Processor.compileQuery(). */
  constructor(processor: Processor, address: number, isUpdating: boolean) {
    this.processor = processor;
    this.handle = address;
    this.isUpdating = isUpdating;
  }

  /** Evaluates the query; the result is a new value. */
  evaluate(options: QueryEvaluateOptions = {}): Value {
    const processor = this.processor;
    const exports = processor.full('XQuery');
    const address = this.address();
    const state = processor.enter();
    const scratch = new Scratch(state);
    const made: Value[] = [];
    let result = 0;
    let failure: unknown;
    let sideResults: SideResult[] = [];
    try {
      const request = scratch.words([
        options.context === undefined ? 0 : processor.input(options.context, made).address(),
        ...processor.bindingWords(scratch, options.variables, made),
        options.updates?.putDocument === undefined ? 0 : 1,
        0,
        0,
      ]);
      result = exports.sxml_javascript_query_evaluate(address, request);
      // Read before a callback can use the processor.
      failure = result === 0 ? processor.failure() : undefined;
      sideResults = processor.sideResults(request / 4 + 4);
    } finally {
      scratch.release();
      for (const value of made) {
        value.dispose();
      }
    }
    const value = result === 0 ? undefined : processor.adopt(result);
    processor.deliverMessages(sideResults, options.messages);
    deliverUpdates(processor, sideResults, options.updates);
    if (value === undefined) {
      throw failure;
    }
    return value;
  }

  /** Serializes a value with the serialization parameters the query's
   *  output declarations give (XQuery 4.0, 5.22: method xml unless they
   *  name another). Value.serialize(options) uses options instead. */
  serialize(value: Value): string {
    const processor = this.processor;
    const exports = processor.full('XQuery');
    processor.enter();
    processor.check(value);
    if (exports.sxml_javascript_query_serialize(this.address(), value.address()) !== status_ok) {
      throw processor.failure();
    }
    return processor.output();
  }

  /** Evaluates the query and serializes its result as serialize() does. */
  run(options: QueryEvaluateOptions = {}): string {
    const result = this.evaluate(options);
    try {
      return this.serialize(result);
    } finally {
      result.dispose();
    }
  }

  dispose(): void {
    if (this.handle !== 0) {
      this.processor.releaseHandle(this, 'query', this.handle);
      this.handle = 0;
    }
  }

  private address(): number {
    if (this.handle === 0) {
      throw disposed('query');
    }
    return this.handle;
  }
}

/** Hands UPDATED_TREE and PUT_DOCUMENT side results to the sink; disposes
 *  what it does not take. */
function deliverUpdates(processor: Processor, results: SideResult[], sink: UpdateSink | undefined): void {
  for (const result of results) {
    if (result.kind === side_result_updated_tree && result.first !== undefined && result.second !== undefined) {
      if (sink?.updatedTree !== undefined) {
        sink.updatedTree({ original: result.first, updated: result.second });
      } else {
        result.first.dispose();
        result.second.dispose();
      }
    } else if (result.kind === side_result_put_document && result.first !== undefined) {
      const node = result.first;
      const parameters = result.second;
      if (sink?.putDocument !== undefined) {
        sink.putDocument({
          uri: result.uri,
          node,
          parameters,
          serialize: (): string => {
            if (parameters !== undefined) {
              return processor.serializeWith(node, parameters);
            }
            const options = processor.value({});
            try {
              return processor.serializeWith(node, options);
            } finally {
              options.dispose();
            }
          },
        });
      } else {
        node.dispose();
        parameters?.dispose();
      }
    }
  }
}

// ------------------------------------------------------------------ XSLT

/** A compiled XSLT 4.0 stylesheet (include/sxml/xslt.h).
 *  Processor.compileStylesheet makes it. */
export class Stylesheet {
  readonly processor: Processor;
  private handle: number;

  /** @internal Use Processor.compileStylesheet(). */
  constructor(processor: Processor, address: number) {
    this.processor = processor;
    this.handle = address;
  }

  /** Runs one transformation (XSLT 4.0, 2.3): applies templates to the
   *  source, or calls options.initialTemplate or options.initialFunction. */
  transform(options: TransformOptions = {}): TransformResult {
    const processor = this.processor;
    const exports = processor.full('XSLT');
    const address = this.address();
    const state = processor.enter();
    const scratch = new Scratch(state);
    const made: Value[] = [];
    const delivery = options.delivery ?? 'serialized';
    let result = 0;
    let failure: unknown;
    let output: string | undefined;
    let sideResults: SideResult[] = [];
    try {
      const input = (data: Input | undefined): number =>
        data === undefined ? 0 : processor.input(data, made).address();
      const args = (options.args ?? []).map(input);
      const serialization = processor.serializationOptions(options.serialization);
      if (serialization !== undefined) {
        made.push(serialization);
      }
      const invocation =
        options.initialFunction !== undefined
          ? invocation_call_function
          : options.initialTemplate !== undefined ||
              (options.source === undefined && options.initialMatchSelection === undefined)
            ? invocation_call_template
            : invocation_apply_templates;
      const unnamedMode = options.initialMode === '#unnamed';
      const request = scratch.words([
        input(options.source),
        input(options.initialMatchSelection),
        ...processor.bindingWords(scratch, options.parameters, made),
        ...processor.bindingWords(scratch, options.templateParameters, made),
        ...processor.bindingWords(scratch, options.tunnelParameters, made),
        args.length > 0 ? scratch.words(args) : 0,
        args.length,
        ...scratch.view(options.baseOutputUri ?? ''),
        serialization?.address() ?? 0,
        ...nameWords(scratch, unnamedMode ? undefined : options.initialMode),
        ...nameWords(scratch, options.initialTemplate),
        ...nameWords(scratch, options.initialFunction),
        invocation,
        result_deliveries[delivery],
        result_deliveries[options.resultDocumentDelivery ?? 'serialized'],
        unnamedMode ? 1 : 0,
        0,
        0,
      ]);
      result = exports.sxml_javascript_stylesheet_transform(address, request);
      // Read before a callback can use the processor.
      failure = result === 0 ? processor.failure() : undefined;
      if (result !== 0 && delivery === 'serialized') {
        output = processor.output();
      }
      sideResults = processor.sideResults(request / 4 + 29);
    } finally {
      scratch.release();
      for (const value of made) {
        value.dispose();
      }
    }
    const value = result === 0 ? undefined : processor.adopt(result);
    processor.deliverMessages(sideResults, options.messages);
    const resultDocuments: ResultDocument[] = [];
    for (const side of sideResults) {
      if (side.kind === side_result_result_document) {
        resultDocuments.push({
          uri: side.uri,
          output: side.first === undefined ? side.text : undefined,
          value: side.first,
        });
      }
    }
    if (value === undefined) {
      for (const document of resultDocuments) {
        document.value?.dispose();
      }
      throw failure;
    }
    if (delivery === 'serialized') {
      value.dispose();
      return { output, value: undefined, resultDocuments };
    }
    return { output: undefined, value, resultDocuments };
  }

  dispose(): void {
    if (this.handle !== 0) {
      this.processor.releaseHandle(this, 'stylesheet', this.handle);
      this.handle = 0;
    }
  }

  private address(): number {
    if (this.handle === 0) {
      throw disposed('stylesheet');
    }
    return this.handle;
  }
}

// ------------------------------------------------------------------- XSD

/** The SCHEMA_REPORT side results as reports. */
function schemaReports(results: SideResult[]): SchemaReport[] {
  return results
    .filter((result) => result.kind === side_result_schema_report)
    .map((result) => ({
      code: result.code,
      description: result.text,
      sourceName: result.uri,
      lineNumber: result.lineNumber,
      columnNumber: result.columnNumber,
    }));
}

/** A compiled XSD 1.1 schema (include/sxml/schema.h).
 *  Processor.compileSchema makes it. */
export class Schema {
  readonly processor: Processor;
  private handle: number;

  /** @internal Use Processor.compileSchema(). */
  constructor(processor: Processor, address: number) {
    this.processor = processor;
    this.handle = address;
  }

  /** Validates a document (XSD 1.1 Part 1, 5.2): XML text, parsed and
   *  validated in one pass so that reports have its lines and columns, or
   *  a document or element node of this processor. An invalid document is
   *  not an exception: see the result's validity and reports. Text that
   *  is not well-formed throws, as Processor.parse does. */
  validate(input: Value | string | Uint8Array, options: ValidateOptions = {}): ValidationResult {
    const processor = this.processor;
    const exports = processor.full('XSD validation');
    const address = this.address();
    if (input instanceof Value) {
      processor.check(input);
    }
    const state = processor.enter();
    const scratch = new Scratch(state);
    let result = 0;
    let failure: unknown;
    let validity: Validity = 'invalid';
    let reports: SchemaReport[] = [];
    try {
      const request = scratch.words([
        input instanceof Value ? input.address() : 0,
        ...(input instanceof Value ? [0, 0] : scratch.view(input)),
        ...scratch.view(options.uri ?? ''),
        options.mode === 'lax' ? 1 : 0,
        ...nameWords(scratch, options.type),
        0,
        0,
        0,
      ]);
      result = exports.sxml_javascript_schema_validate(address, request);
      failure = result === 0 && processor.hasFailed() ? processor.failure() : undefined;
      validity = validities[state.module.words()[request / 4 + 10] ?? 1] ?? 'invalid';
      reports = schemaReports(processor.sideResults(request / 4 + 11));
    } finally {
      scratch.release();
    }
    if (failure !== undefined) {
      throw failure;
    }
    return { validity, reports, document: processor.adoptOptional(result) };
  }

  dispose(): void {
    if (this.handle !== 0) {
      this.processor.releaseHandle(this, 'schema', this.handle);
      this.handle = 0;
    }
  }

  private address(): number {
    if (this.handle === 0) {
      throw disposed('schema');
    }
    return this.handle;
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
