import { closeSync, openSync, writeSync } from 'node:fs';
import { load, textSink } from '@tinyactors/sxml';

const sxml = await load();

// A large source that never exists whole: a generator of chunks, read
// as the document is parsed.
function* orders(count) {
  yield '<orders>';
  for (let start = 1; start <= count; start += 1000) {
    let text = '';
    for (let id = start; id < start + 1000 && id <= count; id += 1) {
      text += `<order id="${id}" total="${(id % 97) + 0.5}"/>`;
    }
    yield text;
  }
  yield '</orders>';
}
const processor = sxml.createProcessor({
  resolver: ({ absoluteUri }) => (absoluteUri === 'urn:orders' ? orders(200_000) : undefined),
});

// A streamable mode reads the source as it arrives; the output goes to a
// file a chunk at a time, written as each chunk is made.
const stylesheet = processor.compileStylesheet(`
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode streamable="yes"/>
  <xsl:output omit-xml-declaration="yes"/>
  <xsl:template match="/orders">
    <large>
      <xsl:for-each select="order[@total > 90]">
        <order id="{@id}">€ <xsl:value-of select="@total"/></order>
      </xsl:for-each>
    </large>
  </xsl:template>
</xsl:stylesheet>`);
const file = openSync('large.xml', 'w');
let byteCount = 0;
stylesheet.transform({
  streamedSource: 'urn:orders',
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
const value = sizes.evaluate('"€uro: 1 €, 2 €"');
const pieces = [];
value.serializeTo(textSink((text) => pieces.push(text)), { method: 'text' });
console.log(pieces);
