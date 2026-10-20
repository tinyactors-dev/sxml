// Streamed output and input of the JavaScript package (D294): sinks that
// receive serialized output a chunk at a time, adapters to WHATWG and
// Node.js streams, and the router that hands the shim's output events
// (bindings/host/sxml_host.h, "Streams") to them.
//
// An evaluation runs synchronously inside sxml.wasm, so a sink is called
// during the call that serializes, once per chunk (64 KiB by default,
// ProcessorOptions.outputChunkByteCount), with a copy of the bytes: the
// buffer in the module is reused for the next chunk. A sink cannot make
// the evaluation wait. Backpressure therefore means only this: a sink that
// consumes each chunk at once (fs.writeSync, a hash, a decoder feeding a
// parser, postMessage from a worker to a thread that reads concurrently)
// keeps memory bounded by one chunk; a sink that queues (a WritableStream,
// a Node.js stream, a ReadableStream's queue) holds the chunks in
// JavaScript memory until the call returns and the event loop drains them,
// which still spares the copy in the module's memory and the one string.

/** Receives serialized output a chunk at a time: a function given each
 *  chunk, or an object whose write is given each chunk, close is called
 *  when the output is complete, and abort when the call fails (with the
 *  error it throws) or the result document was abandoned. Each chunk is
 *  the sink's own copy. A sink must not use the processor; an exception
 *  it throws stops the call, which rethrows it. */
export type OutputSink = ((chunk: Uint8Array) => void) | OutputSinkObject;

export interface OutputSinkObject {
  write(chunk: Uint8Array): void;
  close?(): void;
  abort?(reason: unknown): void;
}

/** A sink that decodes UTF-8 across chunk borders (TextDecoder in stream
 *  mode: a character whose bytes two chunks share is decoded once both
 *  arrived) and hands each decoded piece, never empty, to onText. */
export function textSink(onText: (text: string) => void): OutputSinkObject {
  const decoder = new TextDecoder('utf-8', { fatal: true });
  return {
    write(chunk: Uint8Array): void {
      const text = decoder.decode(chunk, { stream: true });
      if (text !== '') {
        onText(text);
      }
    },
    close(): void {
      const text = decoder.decode();
      if (text !== '') {
        onText(text);
      }
    },
  };
}

/** A sink that writes to a WHATWG WritableStream: each chunk is queued with
 *  the stream's writer at once (the call cannot await the stream), and
 *  the stream is closed, or aborted, after the call. Await done for the
 *  stream to finish. */
export function writableStreamSink(stream: WritableStream<Uint8Array>): OutputSinkObject & {
  readonly done: Promise<void>;
} {
  const writer = stream.getWriter();
  let finish: Promise<void> | undefined;
  const written: Promise<void>[] = [];
  return {
    write(chunk: Uint8Array): void {
      const pending = writer.write(chunk);
      // Failures surface through done.
      pending.catch(() => undefined);
      written.push(pending);
    },
    close(): void {
      finish = Promise.all(written).then(() => writer.close());
    },
    abort(reason: unknown): void {
      finish = writer.abort(reason);
    },
    get done(): Promise<void> {
      return finish ?? Promise.all(written).then(() => undefined);
    },
  };
}

/** A ReadableStream of the output that produce writes to the sink it is
 *  given: produce runs when the stream is first read, and the stream
 *  errors with what it throws. */
export function outputReadableStream(produce: (sink: OutputSink) => void): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(controller): void {
      try {
        produce((chunk) => controller.enqueue(chunk));
        controller.close();
      } catch (exception) {
        controller.error(exception);
      }
    },
  });
}

/** What nodeStreamSink needs of a Node.js Writable. */
export interface NodeWritable {
  write(chunk: Uint8Array): boolean;
  end(): unknown;
  destroy(error?: Error): unknown;
}

/** A sink that writes to a Node.js Writable (a file, an HTTP response):
 *  write() is called for each chunk, whatever it returns (the stream
 *  buffers until the call returns), then end(), or destroy() on failure. */
export function nodeStreamSink(writable: NodeWritable): OutputSinkObject {
  return {
    write(chunk: Uint8Array): void {
      writable.write(chunk);
    },
    close(): void {
      writable.end();
    },
    abort(reason: unknown): void {
      writable.destroy(reason instanceof Error ? reason : new Error(String(reason)));
    },
  };
}

/** Writes a chunk to a sink. @internal */
export function writeChunk(sink: OutputSink, chunk: Uint8Array): void {
  if (typeof sink === 'function') {
    sink(chunk);
  } else {
    sink.write(chunk);
  }
}

/** Ends a sink: close, or abort with reason. @internal */
export function endSink(sink: OutputSink, failed: boolean, reason?: unknown): void {
  if (typeof sink === 'function') {
    return;
  }
  if (failed) {
    sink.abort?.(reason);
  } else {
    sink.close?.();
  }
}

/** sxml_host_output_event_kind. */
const output_event_bytes = 0;
const output_event_result_document_opened = 1;

/** A serialized result document whose bytes were gathered as text, for a
 *  transformation without a sink for it. @internal */
export interface GatheredDocument {
  uri: string;
  output: string;
}

/**
 * Routes one call's output events (sxml_host_output_event) to sinks: bytes
 * go to the principal sink, or to the sink of the result document opened
 * and not yet closed, which openDocument chooses; a document without one
 * is gathered as text. @internal
 */
export class OutputRouter {
  private readonly principal: OutputSink | undefined;
  private readonly openDocument: ((uri: string) => OutputSink | undefined) | undefined;
  private documentUri: string | undefined;
  private documentSink: OutputSink | undefined;
  private documentText: string[] = [];
  private documentDecoder: TextDecoder | undefined;
  /** Every result document's sink the router opened and did not end. */
  private readonly openSinks: OutputSink[] = [];
  readonly gathered: GatheredDocument[] = [];
  /** Without a principal sink, the principal output as text. */
  private readonly principalText: string[] = [];
  private readonly principalDecoder = new TextDecoder();

  constructor(
    principal: OutputSink | undefined,
    openDocument?: (uri: string) => OutputSink | undefined,
  ) {
    this.principal = principal;
    this.openDocument = openDocument;
  }

  /** One event; bytes is a view into the module's memory. */
  event(kind: number, bytes: Uint8Array, isAbandoned: boolean, uri: () => string): void {
    if (kind === output_event_bytes) {
      if (this.documentUri === undefined) {
        if (this.principal !== undefined) {
          writeChunk(this.principal, bytes.slice());
        } else {
          this.principalText.push(this.principalDecoder.decode(bytes, { stream: true }));
        }
      } else if (this.documentSink !== undefined) {
        writeChunk(this.documentSink, bytes.slice());
      } else {
        this.documentText.push(this.documentDecoder?.decode(bytes, { stream: true }) ?? '');
      }
      return;
    }
    if (kind === output_event_result_document_opened) {
      this.documentUri = uri();
      this.documentSink = this.openDocument?.(this.documentUri);
      if (this.documentSink === undefined) {
        this.documentDecoder = new TextDecoder();
        this.documentText = [];
      } else {
        this.openSinks.push(this.documentSink);
      }
      return;
    }
    // RESULT_DOCUMENT_CLOSED.
    const sink = this.documentSink;
    if (sink !== undefined) {
      this.openSinks.splice(this.openSinks.indexOf(sink), 1);
      endSink(sink, isAbandoned, new Error(`sxml: the result document ${this.documentUri ?? ''} was abandoned`));
    } else if (!isAbandoned) {
      this.documentText.push(this.documentDecoder?.decode() ?? '');
      this.gathered.push({ uri: this.documentUri ?? '', output: this.documentText.join('') });
    }
    this.documentUri = undefined;
    this.documentSink = undefined;
    this.documentDecoder = undefined;
    this.documentText = [];
  }

  /** The principal output gathered without a principal sink. */
  principalOutput(): string {
    this.principalText.push(this.principalDecoder.decode());
    return this.principalText.splice(0).join('');
  }

  /** Ends the principal sink and any result document left open. */
  finish(failed: boolean, reason?: unknown): void {
    for (const sink of this.openSinks.splice(0)) {
      endSink(sink, true, reason);
    }
    if (this.principal !== undefined) {
      endSink(this.principal, failed, reason);
    }
  }
}

/** A synchronous source of a document's bytes, as a resolver may answer
 *  for kind 'xml-document' (D294): an iterable of chunks, such as a
 *  generator that reads a file with fs.readSync. Each chunk is
 *  Uint8Array or string (encoded as UTF-8). It is read as the document is
 *  parsed: a document XSLT streams (TransformOptions.streamedSource,
 *  xsl:source-document streamable="yes") is never whole in memory; fn:doc
 *  reads it whole first. It is closed (return()) when no longer read. */
export type ChunkSource = Iterable<Uint8Array | string>;

/** Reads a chunk source into the module's buffers. @internal */
export class ChunkReader {
  private readonly iterator: Iterator<Uint8Array | string>;
  private pending: Uint8Array = new Uint8Array(0);
  private isDone = false;

  constructor(source: ChunkSource) {
    this.iterator = source[Symbol.iterator]();
  }

  /** Fills the front of buffer; returns [byte count, is end]. */
  read(buffer: Uint8Array): [number, boolean] {
    let filled = 0;
    while (filled < buffer.byteLength) {
      if (this.pending.byteLength === 0) {
        if (this.isDone) {
          break;
        }
        const next = this.iterator.next();
        if (next.done === true) {
          this.isDone = true;
          break;
        }
        this.pending = typeof next.value === 'string' ? new TextEncoder().encode(next.value) : next.value;
        continue;
      }
      const count = Math.min(this.pending.byteLength, buffer.byteLength - filled);
      buffer.set(this.pending.subarray(0, count), filled);
      this.pending = this.pending.subarray(count);
      filled += count;
    }
    return [filled, this.isDone && this.pending.byteLength === 0];
  }

  /** Closes the source. */
  close(): void {
    if (!this.isDone) {
      this.isDone = true;
      this.iterator.return?.();
    }
  }
}

/** Reads a whole chunk source, for the kinds a byte reader cannot
 *  deliver. @internal */
export function readWhole(source: ChunkSource): Uint8Array {
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (const chunk of source) {
    const bytes = typeof chunk === 'string' ? new TextEncoder().encode(chunk) : chunk;
    chunks.push(bytes);
    total += bytes.byteLength;
  }
  const whole = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    whole.set(chunk, at);
    at += chunk.byteLength;
  }
  return whole;
}
