// sxml for JavaScript: XML parsing, XPath 4.0, XQuery 4.0, XSLT 4.0, XSD 1.1
// validation and serialization in Node.js and browsers, over sxml.wasm
// (docs/DECISIONS.md, D10, D138, D272).
//
//     import { load } from '@tinyactors/sxml';
//     const sxml = await load();
//     const processor = sxml.createProcessor();
//     const book = processor.parse('<book><title>XPath</title></book>');
//     processor.evaluate('//title/string()', { context: book }).toString();
//     // '"XPath"' (the adaptive method of Serialization 4.0)
//
// The one module has every language and every optional feature (D272):
// XQuery 4.0 (Processor.compileQuery), XSLT 4.0 (Processor.compileStylesheet)
// and XSD 1.1 validation (Processor.compileSchema) besides XPath.
//
// Every object that owns memory in the module (Processor, XPathExpression,
// XQuery, Stylesheet, Schema, Value) has dispose(); one that is never disposed is released when it is
// garbage collected (FinalizationRegistry). The C side is the shim in
// ../sxml_javascript.c; the word offsets below follow its structs.

import { linkLanguageModule } from './linker.ts';
import {
  type ChunkSource,
  ChunkReader,
  type GatheredDocument,
  type OutputSink,
  OutputRouter,
  readWhole,
} from './stream.ts';
import { wasiImports } from './wasi.ts';

export {
  type ChunkSource,
  type NodeWritable,
  nodeStreamSink,
  type OutputSink,
  type OutputSinkObject,
  outputReadableStream,
  textSink,
  writableStreamSink,
} from './stream.ts';

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
  /** Default: sxml.wasm next to this file, which has every language and
   *  every optional feature (Sxml.features). */
  wasm?: WasmSource;
  /** The DUCET for the uca-collation feature (D213), the
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
  /** A file of the IANA Time Zone Database for the
   *  timezone-database feature: absoluteUri is 'urn:sxml:timezone-data:'
   *  and a zoneinfo path, a zone name ('Europe/Paris', a TZif file) or
   *  'zone1970.tab'. Answer with the file's bytes. */
  | 'timezone-data'
  /** The DUCET for the uca-collation feature: absoluteUri
   *  is 'http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt'. Answer
   *  with the file's text, or leave it to LoadOptions.collationData. */
  | 'collation-data'
  /** A CLDR file for the localization feature: absoluteUri
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
 *  and is rethrown by the call that started it. An XML document may also
 *  be a ChunkSource, read a chunk at a time as it is parsed (D294). */
export type Resolver = (request: ResourceRequest) => string | Uint8Array | ChunkSource | null | undefined;

export interface ProcessorOptions {
  /** Default: nothing is available. */
  resolver?: Resolver;
  /** The optional features the processor may use, by name, each one of
   *  Sxml.features. Default: every feature of the module. Without a
   *  feature, sxml behaves as the specifications say a processor without
   *  it does. */
  features?: string[];
  /** tree-sitter languages for sxml:parse-code beside the built-in ones,
   *  loaded with Sxml.loadCodeLanguage; one with a built-in language's
   *  name replaces it. Default: none. */
  codeLanguages?: CodeLanguage[];
  /** The size of the chunks an OutputSink receives, in bytes; default
   *  65,536 (D294). */
  outputChunkByteCount?: number;
  /** The most evaluation steps one evaluation may take before it stops
   *  with an error (the C processor limit maximum_evaluation_step_count):
   *  a whole number up to 2^53; default, and 0, sxml's 100,000,000, a few
   *  seconds of work. A streamed document grants 500 steps more for each
   *  element it reads, so long streams run within it (D295). */
  maximumEvaluationStepCount?: number;
  /** For tests: fail this allocation (counted from 1) with out of memory. */
  failingAllocationNumber?: number;
}

/** A tree-sitter language file to load (Sxml.loadCodeLanguage). */
export interface CodeLanguageOptions {
  /** The language file, as `tree-sitter build --wasm` writes it
   *  (tree-sitter-NAME.wasm): a URL or path, the bytes, a compiled
   *  module, or a fetch Response. */
  wasm: WasmSource;
  /** The language's name in sxml:parse-code: ASCII letters, digits, '_',
   *  '-', '+' and '.'. Default: NAME of the file's tree_sitter_NAME
   *  function. */
  name?: string;
  /** The text of the grammar's queries/highlights.scm; default none, so
   *  no highlight attributes. */
  highlights?: string;
  /** The text of queries/locals.scm; default none. */
  locals?: string;
  /** The text of queries/injections.scm; default none. */
  injections?: string;
  /** tree-sitter.json's injection-regex, which other languages'
   *  injections match to mean this one; default: only its name. */
  injectionRegex?: string;
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
   *  array, math, err, ...) and to those of the processor's features (bin
   *  with expath-binary), which they may rebind. */
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

// ----------------------------------------------------------------- XQuery

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
  /** The same, written to sink a chunk at a time (D294). */
  serializeTo(sink: OutputSink): void;
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

// ------------------------------------------------------------------- XSLT

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
  /** With delivery 'serialized': receives the principal result a chunk at
   *  a time as it is serialized, instead of TransformResult.output (D294). */
  output?: OutputSink;
  /** With resultDocumentDelivery 'serialized': chooses the sink of each
   *  result document by its URI, as it begins; undefined gathers it as
   *  ResultDocument.output. A document whose transformation fails while it
   *  is open is aborted. Documents with a sink are not in
   *  TransformResult.resultDocuments. */
  resultDocumentOutput?: (uri: string) => OutputSink | undefined;
  /** The absolute URI of a principal source to stream (XSLT 4.0, 2.4.3;
   *  D228): the resolver is asked for it (kind 'xml-document') and may
   *  answer with a ChunkSource. When the initial mode is streamable, its
   *  nodes are read as they are parsed and never held whole; otherwise it
   *  is built first. Applies templates; not with source or
   *  initialMatchSelection. */
  streamedSource?: string;
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

// -------------------------------------------------------------------- XSD

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
  sxml_javascript_processor_create(request: number): number;
  sxml_javascript_check_code_language(language: number, report: number): number;
  // For language modules (src/linker.ts, D293): the function table, the
  // stack pointer and the C library functions of external scanners.
  __indirect_function_table: WebAssembly.Table;
  __stack_pointer: WebAssembly.Global;
  malloc(byteCount: number): number;
  free(address: number): void;
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
  // XQuery, XSLT and XSD (sxml_javascript.h, "Full module").
  sxml_javascript_query_compile(processor: number, request: number): number;
  sxml_javascript_query_destroy(query: number): void;
  sxml_javascript_query_is_updating(query: number): number;
  sxml_javascript_query_evaluate(query: number, request: number): number;
  sxml_javascript_query_serialize(query: number, value: number): number;
  sxml_javascript_stylesheet_compile(processor: number, request: number): number;
  sxml_javascript_stylesheet_destroy(stylesheet: number): void;
  sxml_javascript_stylesheet_transform(stylesheet: number, request: number): number;
  sxml_javascript_schema_create(processor: number): number;
  sxml_javascript_schema_destroy(schema: number): void;
  sxml_javascript_schema_compile(schema: number, request: number): number;
  sxml_javascript_schema_validate(schema: number, request: number): number;
  // Streams (sxml_javascript_stream.c, D294).
  sxml_javascript_set_output(processor: number, chunkByteCount: number): number;
  sxml_javascript_set_byte_reader(processor: number): number;
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
/** The conventional prefixes of the optional features' namespaces, bound
 *  for XPath and XQuery wherever the feature is present, as the sxml
 *  command binds them (EXPath Binary 4.0, 1.5; EXPath File 4.0, 1.7;
 *  D235): feature name, prefix, namespace URI. */
const featurePrefixes: readonly (readonly [string, string, string])[] = [
  ['expath-binary', 'bin', 'http://expath.org/ns/binary'],
  ['source-code', 'sxml', 'urn:sxml'],
  ['expath-file', 'file', 'http://expath.org/ns/file'],
];

class ProcessorState {
  readonly module: Sxml;
  address: number;
  readonly resolver: Resolver | undefined;
  /** Handles not yet released: address to kind. */
  readonly live = new Map<number, HandleKind>();
  /** The exception the resolver threw during the current call. */
  pendingException: unknown = undefined;
  isInResolver = false;
  /** Whether an output sink or chunk source is running (D294). */
  isInSink = false;
  /** The router of the current streaming call's output (D294). */
  outputRouter: OutputRouter | undefined = undefined;
  /** The chunk sources the resolver delivered, by reader identifier. */
  readonly readers = new Map<number, ChunkReader>();
  nextReaderIdentifier = 1;
  /** ProcessorOptions.outputChunkByteCount. */
  outputChunkByteCount = default_output_chunk_byte_count;
  /** parse-json($json), compiled when first needed. */
  jsonExpression = 0;
  /** What sxml_javascript_processor_destroy found still allocated. */
  leakedAllocationCount = 0;
  /** The processor's sxml_source_code_language_view and its array, made
   *  with malloc, which must outlive it; 0 for none. */
  codeLanguageView = 0;
  /** The prefixes of the processor's features' namespaces (D235). */
  readonly featureNamespaces: Readonly<Record<string, string>>;

  constructor(module: Sxml, address: number, resolver: Resolver | undefined, features: readonly string[]) {
    this.module = module;
    this.address = address;
    this.resolver = resolver;
    const namespaces: Record<string, string> = {};
    for (const [feature, prefix, uri] of featurePrefixes) {
      if (features.includes(feature)) {
        namespaces[prefix] = uri;
      }
    }
    this.featureNamespaces = Object.freeze(namespaces);
  }

  /** The namespaces of a compilation: the features' prefixes, then the
   *  caller's, which may rebind them. */
  staticNamespaces(namespaces: Record<string, string> | undefined): [string, string][] {
    return Object.entries({ ...this.featureNamespaces, ...(namespaces ?? {}) });
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
    if (this.codeLanguageView !== 0) {
      this.module.exports.free(this.codeLanguageView);
      this.codeLanguageView = 0;
    }
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
  /** The optional features of the module: all of them (D272; 'test' adds
   *  nothing, it checks that the list reaches the processor). */
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
    const stepCount = options.maximumEvaluationStepCount ?? 0;
    if (!Number.isSafeInteger(stepCount) || stepCount < 0) {
      throw new RangeError('sxml: maximumEvaluationStepCount must be a whole number from 0 to 2^53 - 1');
    }
    const codeLanguageView = this.codeLanguageView(options.codeLanguages ?? []);
    // sxml_javascript_processor_request (sxml_javascript.h): four words
    // and the step count's two, low first.
    const request = this.exports.malloc(processor_request_byte_count);
    if (request === 0) {
      if (codeLanguageView !== 0) {
        this.exports.free(codeLanguageView);
      }
      throw outOfMemory();
    }
    this.words().set(
      [options.failingAllocationNumber ?? 0, featureMask >>> 0, codeLanguageView, 0, stepCount % 2 ** 32, Math.floor(stepCount / 2 ** 32)],
      request / 4,
    );
    const address = this.exports.sxml_javascript_processor_create(request);
    this.exports.free(request);
    if (address === 0) {
      if (codeLanguageView !== 0) {
        this.exports.free(codeLanguageView);
      }
      throw outOfMemory();
    }
    const state = new ProcessorState(this, address, options.resolver, options.features ?? this.features);
    state.codeLanguageView = codeLanguageView;
    this.exports.sxml_javascript_set_byte_reader(address);
    if (options.outputChunkByteCount !== undefined) {
      if (!Number.isInteger(options.outputChunkByteCount) || options.outputChunkByteCount < 1) {
        throw new RangeError('sxml: outputChunkByteCount must be a positive integer');
      }
      state.outputChunkByteCount = options.outputChunkByteCount;
    }
    this.processors.set(address, state);
    const processor = new Processor(state);
    this.track(processor, { processor: state, kind: 'processor', address });
    return processor;
  }

  /**
   * Loads a tree-sitter language file (tree-sitter-NAME.wasm, as
   * `tree-sitter build --wasm` writes it) into this module for
   * sxml:parse-code: links it into sxml.wasm (src/linker.ts) and checks
   * its ABI and queries. Give it to processors with createProcessor({
   * codeLanguages }). It stays loaded as long as this Sxml. Rejects with
   * an Error that says what is wrong with the file, the name or a query.
   */
  async loadCodeLanguage(options: CodeLanguageOptions): Promise<CodeLanguage> {
    const exports = this.exports;
    const module = await compileModule(options.wasm);
    const functions = new Map<string, Function>();
    for (const [name, value] of Object.entries(exports)) {
      // The C library functions the build exports for external scanners
      // (bindings/javascript/CMakeLists.txt); sxml's own are sxml_.
      if (typeof value === 'function' && !name.startsWith('sxml_') && !name.startsWith('_')) {
        functions.set(name, value);
      }
    }
    const linked = await linkLanguageModule(
      module,
      {
        memory: exports.memory,
        table: exports.__indirect_function_table,
        stackPointer: exports.__stack_pointer,
        functions,
        reserve: (byteCount, alignment) => {
          const address = this.reserve(byteCount + alignment - 1);
          const aligned = Math.ceil(address / alignment) * alignment;
          this.bytes().fill(0, aligned, aligned + byteCount);
          return aligned;
        },
      },
      options.name,
    );
    const name = options.name ?? linked.functionName;
    // sxml_source_code_language (processor.h): six byte views but the
    // grammar, eleven words; the texts stay with the language.
    const view = (text: string | undefined): [number, number] => {
      const bytes = encoder.encode(text ?? '');
      const address = this.reserve(bytes.byteLength);
      this.bytes().set(bytes, address);
      return [address, bytes.byteLength];
    };
    const words = [
      ...view(name),
      linked.languageAddress,
      ...view(options.highlights),
      ...view(options.locals),
      ...view(options.injections),
      ...view(options.injectionRegex),
    ];
    const language = this.reserve(code_language_byte_count);
    this.words().set(words, language / 4);
    // sxml_source_code_language_report: problem, abi_version, byte_offset.
    const report = this.reserve(12);
    const status = exports.sxml_javascript_check_code_language(language, report);
    const [problem = 0, abiVersion = 0, byteOffset = 0] = this.words().subarray(report / 4, report / 4 + 3);
    exports.free(report);
    if (status === status_out_of_memory) {
      throw outOfMemory();
    }
    if (status !== status_ok) {
      throw new Error(codeLanguageProblem(problem, { name, abiVersion, byteOffset }));
    }
    return new CodeLanguage(this, language, name, abiVersion);
  }

  /** malloc of sxml.wasm, for memory that outlives processors.
   *  @internal */
  reserve(byteCount: number): number {
    const address = this.exports.malloc(Math.max(byteCount, 1));
    if (address === 0) {
      throw outOfMemory();
    }
    return address;
  }

  /** A processor's sxml_source_code_language_view: the view, then a copy
   *  of each language; 0 for none. @internal */
  codeLanguageView(languages: readonly CodeLanguage[]): number {
    if (languages.length === 0) {
      return 0;
    }
    const names = new Set<string>();
    for (const language of languages) {
      if (language.module !== this) {
        throw new Error('sxml: this code language was loaded by another sxml module');
      }
      if (names.has(language.name)) {
        throw new Error(`sxml: two code languages are named '${language.name}'`);
      }
      names.add(language.name);
    }
    const view = this.reserve(8 + languages.length * code_language_byte_count);
    const bytes = this.bytes();
    languages.forEach((language, index) => {
      const at = view + 8 + index * code_language_byte_count;
      bytes.copyWithin(at, language.address, language.address + code_language_byte_count);
    });
    this.words().set([view + 8, languages.length], view / 4);
    return view;
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
    let answer: string | Uint8Array | ChunkSource | null | undefined;
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
    if (typeof answer !== 'string' && !(answer instanceof Uint8Array)) {
      // A chunk source (D294): read as it is parsed for an XML document,
      // whole for the other kinds.
      if (resourceRequest.kind === 'xml-document') {
        const identifier = state.nextReaderIdentifier;
        state.nextReaderIdentifier += 1;
        state.readers.set(identifier, new ChunkReader(answer));
        this.words().set([resource_form_byte_reader, 0, 0, identifier], resourceAddress / 4);
        return status_ok;
      }
      try {
        state.isInResolver = true;
        answer = readWhole(answer);
      } catch (exception) {
        state.pendingException = exception;
        return status_input_output_failed;
      } finally {
        state.isInResolver = false;
      }
    }
    const bytes = typeof answer === 'string' ? encoder.encode(answer) : answer;
    const address = this.exports.sxml_javascript_allocate(processorAddress, bytes.byteLength);
    if (address === 0) {
      return status_out_of_memory;
    }
    this.bytes().set(bytes, address);
    this.words().set([1, address, bytes.byteLength, 0], resourceAddress / 4);
    return status_ok;
  }

  /** The import sxml_host.write (sxml_host_output_event: kind, bytes,
   *  is_abandoned). @internal */
  write(processorAddress: number, eventAddress: number): number {
    const state = this.processors.get(processorAddress);
    const router = state?.outputRouter;
    if (state === undefined || router === undefined) {
      return status_ok;
    }
    const [kind = 0, address = 0, byteCount = 0, isAbandoned = 0] = this.words().subarray(
      eventAddress / 4,
      eventAddress / 4 + 4,
    );
    state.isInSink = true;
    try {
      router.event(kind, this.bytes().subarray(address, address + byteCount), isAbandoned !== 0, () =>
        this.text(address, byteCount),
      );
      return status_ok;
    } catch (exception) {
      state.pendingException = exception;
      return status_input_output_failed;
    } finally {
      state.isInSink = false;
    }
  }

  /** The import sxml_host.read (sxml_host_read_request: identifier,
   *  buffer, byte_count, is_end). @internal */
  read(processorAddress: number, requestAddress: number): number {
    const state = this.processors.get(processorAddress);
    const at = requestAddress / 4;
    const [identifier = 0, buffer = 0, bufferByteCount = 0] = this.words().subarray(at, at + 3);
    const reader = state?.readers.get(identifier);
    if (state === undefined || reader === undefined) {
      this.words().set([0, 1], at + 3);
      return status_ok;
    }
    state.isInSink = true;
    try {
      const [byteCount, isEnd] = reader.read(this.bytes().subarray(buffer, buffer + bufferByteCount));
      this.words().set([byteCount, isEnd ? 1 : 0], at + 3);
      return status_ok;
    } catch (exception) {
      state.pendingException = exception;
      return status_input_output_failed;
    } finally {
      state.isInSink = false;
    }
  }

  /** The import sxml_host.release_reader. @internal */
  releaseReader(processorAddress: number, identifier: number): void {
    const state = this.processors.get(processorAddress);
    const reader = state?.readers.get(identifier);
    if (state === undefined || reader === undefined) {
      return;
    }
    state.readers.delete(identifier);
    try {
      reader.close();
    } catch (exception) {
      state.pendingException ??= exception;
    }
  }
}

/** ProcessorOptions.outputChunkByteCount's default. */
const default_output_chunk_byte_count = 65536;

/** SXML_RESOURCE_FORM_BYTE_READER (include/sxml/processor.h). */
const resource_form_byte_reader = 4;

/** The size of sxml_source_code_language in sxml.wasm (checked by the
 *  shim, sxml_javascript_exports.c). */
const code_language_byte_count = 44;
/** sizeof(sxml_javascript_processor_request) on wasm32 (D295). */
const processor_request_byte_count = 24;

/** sxml_source_code_language_problem, as the sxml command words it
 *  (apps/sxml/code_languages.c). */
function codeLanguageProblem(
  problem: number,
  { name, abiVersion, byteOffset }: { name: string; abiVersion: number; byteOffset: number },
): string {
  const queries = ['highlights', 'locals', 'injections'];
  if (problem === 1) {
    return `sxml: '${name}' is no language name (ASCII letters, digits, '_', '-', '+' and '.')`;
  }
  if (problem === 2) {
    return `sxml: the grammar's ABI is ${abiVersion}; sxml reads 13 to 15`;
  }
  return `sxml: the ${queries[problem - 3] ?? 'highlights'} query does not fit the grammar at byte ${byteOffset}`;
}

/** A tree-sitter language loaded by Sxml.loadCodeLanguage, for
 *  createProcessor({ codeLanguages }). */
export class CodeLanguage {
  /** @internal */
  readonly module: Sxml;
  /** @internal Its sxml_source_code_language in the module's memory. */
  readonly address: number;
  /** Its name in sxml:parse-code. */
  readonly name: string;
  /** The grammar's tree-sitter language ABI, 13 to 15. */
  readonly abiVersion: number;

  /** @internal Use Sxml.loadCodeLanguage(). */
  constructor(module: Sxml, address: number, name: string, abiVersion: number) {
    this.module = module;
    this.address = address;
    this.name = name;
    this.abiVersion = abiVersion;
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
      write: (processor: number, event: number): number =>
        sxml === undefined ? status_ok : sxml.write(processor, event),
      read: (processor: number, request: number): number =>
        sxml === undefined ? status_ok : sxml.read(processor, request),
      release_reader: (processor: number, identifier: number): void =>
        sxml?.releaseReader(processor, identifier),
    },
  });
  const exports = instance.exports as unknown as Exports;
  if (typeof exports.sxml_javascript_query_compile !== 'function') {
    // A module of an older package, without XQuery, XSLT and XSD.
    throw new Error('sxml: this module is not the package\'s sxml.wasm');
  }
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
   *  evaluating it many times. */
  compileQuery(query: string, options: QueryCompileOptions = {}): XQuery {
    const exports = this.exports();
    const state = this.enter();
    const scratch = new Scratch(state);
    let address: number;
    try {
      const namespaceWords: number[] = [];
      for (const [prefix, uri] of state.staticNamespaces(options.namespaces)) {
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
   *  'xslt-package'), for running it many times. */
  compileStylesheet(stylesheet: string | Uint8Array, options: StylesheetCompileOptions = {}): Stylesheet {
    const exports = this.exports();
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
   *  schema has errors. */
  compileSchema(
    documents: string | SchemaDocument | (string | SchemaDocument)[],
    options: SchemaCompileOptions = {},
  ): Schema {
    const exports = this.exports();
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

  /** Serializes a value as serialize() does, writing the output to sink a
   *  chunk at a time instead of returning it (D294). */
  serializeTo(value: Value, sink: OutputSink, options?: SerializationOptions): void {
    this.enter();
    this.check(value);
    const optionsValue = this.serializationOptions(options);
    try {
      this.serializeWith(value, optionsValue, sink);
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
    if (this.state.isInSink) {
      throw new Error('sxml: an output sink or chunk source must not use its processor');
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
      for (const [prefix, uri] of state.staticNamespaces(options.namespaces)) {
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
   *  by the adaptive method without; to sink when given. */
  serializeWith(value: Value, options: Value | undefined): string;
  serializeWith(value: Value, options: Value | undefined, sink: OutputSink): void;
  serializeWith(value: Value, options: Value | undefined, sink?: OutputSink): string | void {
    const state = this.enter();
    const scratch = new Scratch(state);
    try {
      const request = scratch.words([value.address(), options?.address() ?? 0]);
      const status = this.streaming(new OutputRouter(sink), sink !== undefined, () =>
        state.module.exports.sxml_javascript_serialize(state.address, request),
      );
      if (status !== status_ok) {
        throw this.failure();
      }
      return sink === undefined ? this.output() : undefined;
    } finally {
      scratch.release();
    }
  }

  /** @internal Runs call, a call of the shim that serializes, with its
   *  output written to router's sinks when streams is true (D294); ends
   *  them as the call ends, failed when it returned failed (0 or a status
   *  other than OK, as isFailure says) or threw. */
  streaming<T>(router: OutputRouter, streams: boolean, call: () => T, isFailure?: (result: T) => boolean): T {
    if (!streams) {
      return call();
    }
    const state = this.state;
    const exports = state.module.exports;
    exports.sxml_javascript_set_output(state.address, state.outputChunkByteCount);
    state.outputRouter = router;
    let result: T;
    try {
      result = call();
    } catch (exception) {
      router.finish(true, exception);
      throw exception;
    } finally {
      state.outputRouter = undefined;
      exports.sxml_javascript_set_output(state.address, 0);
    }
    const failed = isFailure !== undefined ? isFailure(result) : result !== status_ok;
    if (failed) {
      // The failure stays readable: failure() reads it, and the sinks are
      // aborted with it after the caller has.
      const reason = this.peekFailure();
      router.finish(true, reason);
    } else {
      router.finish(false);
    }
    return result;
  }

  /** @internal The last failure without taking the resolver's exception. */
  peekFailure(): unknown {
    if (this.state.pendingException !== undefined) {
      return this.state.pendingException;
    }
    return this.failureRecord();
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

  /** @internal The module's exports. */
  exports(): Exports {
    return this.state.module.exports;
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
    return this.failureRecord();
  }

  /** @internal The processor's error as an SxmlError. */
  failureRecord(): SxmlError {
    const state = this.state;
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
    const exports = processor.exports();
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
    this.serializeOutput(value, undefined);
    return this.processor.output();
  }

  /** Serializes a value as serialize() does, writing the output to sink a
   *  chunk at a time instead of returning it (D294). */
  serializeTo(value: Value, sink: OutputSink): void {
    this.serializeOutput(value, sink);
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

  /** Evaluates the query and writes its serialized result to sink a chunk
   *  at a time, as serializeTo() does (D294). */
  runTo(sink: OutputSink, options: QueryEvaluateOptions = {}): void {
    const result = this.evaluate(options);
    try {
      this.serializeTo(result, sink);
    } finally {
      result.dispose();
    }
  }

  private serializeOutput(value: Value, sink: OutputSink | undefined): void {
    const processor = this.processor;
    const exports = processor.exports();
    processor.enter();
    processor.check(value);
    const address = this.address();
    const status = processor.streaming(new OutputRouter(sink), sink !== undefined, () =>
      exports.sxml_javascript_query_serialize(address, value.address()),
    );
    if (status !== status_ok) {
      throw processor.failure();
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
          serializeTo: (sink: OutputSink): void => {
            if (parameters !== undefined) {
              processor.serializeWith(node, parameters, sink);
              return;
            }
            const options = processor.value({});
            try {
              processor.serializeWith(node, options, sink);
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
    const exports = processor.exports();
    const address = this.address();
    const state = processor.enter();
    const scratch = new Scratch(state);
    const made: Value[] = [];
    const delivery = options.delivery ?? 'serialized';
    const resultDocumentDelivery = options.resultDocumentDelivery ?? 'serialized';
    if (options.output !== undefined && delivery !== 'serialized') {
      throw new Error("sxml: TransformOptions.output needs delivery 'serialized'");
    }
    if (
      options.streamedSource !== undefined &&
      (options.source !== undefined || options.initialMatchSelection !== undefined)
    ) {
      throw new Error('sxml: TransformOptions.streamedSource is the initial match selection');
    }
    // With a sink, the serialized results are written in chunks (D294).
    const router = new OutputRouter(options.output, options.resultDocumentOutput);
    const streams =
      options.output !== undefined ||
      (options.resultDocumentOutput !== undefined && resultDocumentDelivery === 'serialized');
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
              (options.source === undefined &&
                options.initialMatchSelection === undefined &&
                options.streamedSource === undefined)
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
        result_deliveries[resultDocumentDelivery],
        unnamedMode ? 1 : 0,
        0,
        0,
        ...scratch.view(options.streamedSource ?? ''),
      ]);
      result = processor.streaming(
        router,
        streams,
        () => exports.sxml_javascript_stylesheet_transform(address, request),
        (made) => made === 0,
      );
      // Read before a callback can use the processor.
      failure = result === 0 ? processor.failure() : undefined;
      if (result !== 0 && delivery === 'serialized' && options.output === undefined) {
        output = streams ? router.principalOutput() : processor.output();
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
    const resultDocuments: ResultDocument[] = router.gathered.map((document: GatheredDocument) => ({
      uri: document.uri,
      output: document.output,
      value: undefined,
    }));
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
    const exports = processor.exports();
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

  /** Serializes the value to sink a chunk at a time
   *  (Processor.serializeTo). */
  serializeTo(sink: OutputSink, options?: SerializationOptions): void {
    this.processor.serializeTo(this, sink, options);
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
