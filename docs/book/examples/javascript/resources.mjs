import { readFileSync } from 'node:fs';
import { load, SxmlError } from '@tinyactors/sxml';

const sxml = await load();

// The resolver is the only way an expression reaches anything outside.
// This one serves one trace file, read once, under a URI of our choice.
const files = new Map([
  ['https://traces.example.org/staging/before.json', readFileSync('before.json', 'utf8')],
]);
const processor = sxml.createProcessor({
  resolver: ({ kind, absoluteUri }) => (kind === 'text' ? files.get(absoluteUri) : undefined),
});

const services = processor.evaluate(
  'json-doc("before.json")?resourceSpans?*?resource?attributes?*[?key = "service.name"]?value?stringValue',
  { baseUri: 'https://traces.example.org/staging/' });
console.log(services.toString().split('\n'));

// Errors carry the code, the description and the location.
try {
  processor.evaluate('json-doc("after.json")', { baseUri: 'https://traces.example.org/staging/' });
} catch (error) {
  if (!(error instanceof SxmlError)) throw error;
  console.log(error.code, error.lineNumber, error.columnNumber);
}

processor.dispose();
