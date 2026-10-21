import { closeSync, openSync, writeSync } from 'node:fs';
import { load, textSink } from '@tinyactors/sxml';

const sxml = await load();

// A large trace archive that never exists whole: a generator of chunks
// in the normal form of the traces project, read as it is parsed.
function* archive(count) {
  yield '<traces>';
  for (let start = 1; start <= count; start += 1000) {
    let text = '';
    for (let trace = start; trace < start + 1000 && trace <= count; trace += 1) {
      const query = 5 + ((trace * 7919) % 61);
      text += `<trace id="${trace}" route="GET /api/getFlags">`
        + `<span service="storefront" kind="client" duration="${query + 12}" peer="vane">`
        + `<span service="vane" kind="server" duration="${query + 10}">`
        + `<span service="vane" kind="client" duration="${query}" peer="postgresql" db="postgresql"/>`
        + '</span></span></trace>';
    }
    yield text;
  }
  yield '</traces>';
}
const processor = sxml.createProcessor({
  resolver: ({ absoluteUri }) => (absoluteUri === 'urn:archive' ? archive(200_000) : undefined),
});

// A streamable mode reads the source as it arrives; the output goes to a
// file a chunk at a time, written as each chunk is made.
const stylesheet = processor.compileStylesheet(`
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode streamable="yes"/>
  <xsl:output omit-xml-declaration="yes"/>
  <xsl:template match="/traces">
    <slow>
      <xsl:for-each select="trace/span[@duration > 70]">
        <trace id="{../@id}">≥ 70 ms</trace>
      </xsl:for-each>
    </slow>
  </xsl:template>
</xsl:stylesheet>`);
const file = openSync('slow.xml', 'w');
let byteCount = 0;
stylesheet.transform({
  streamedSource: 'urn:archive',
  output: (chunk) => {
    byteCount += chunk.byteLength;
    writeSync(file, chunk);
  },
});
closeSync(file);
console.log(byteCount, 'bytes;', (sxml.exports.memory.buffer.byteLength / 2 ** 20).toFixed(0), 'MiB of module memory');

// Text a chunk at a time: a character split between two chunks is decoded
// once both arrived.
const sizes = sxml.createProcessor({ outputChunkByteCount: 4 });
const value = sizes.evaluate('"≥ 70 ms: 12 €"');
const pieces = [];
value.serializeTo(textSink((text) => pieces.push(text)), { method: 'text' });
console.log(pieces);
