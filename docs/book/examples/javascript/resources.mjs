import { load, SxmlError } from '@tinyactors/sxml';

const sxml = await load();

// The resolver is the only way an expression reaches anything outside.
const files = new Map([
  ['https://example.org/data/prices.xml',
   '<prices><item name="tea">3.50</item><item name="coffee">4.20</item></prices>'],
]);
const processor = sxml.createProcessor({
  resolver: ({ kind, absoluteUri }) => (kind === 'xml-document' ? files.get(absoluteUri) : undefined),
});

const total = processor.evaluate('sum(doc("prices.xml")//item ! xs:decimal(.))', {
  baseUri: 'https://example.org/data/',
});
console.log(total.toString());

// Errors carry the code, the description and the location.
try {
  processor.evaluate('doc("secret.xml")', { baseUri: 'https://example.org/data/' });
} catch (error) {
  if (!(error instanceof SxmlError)) throw error;
  console.log(error.code, error.lineNumber, error.columnNumber);
}

processor.dispose();
