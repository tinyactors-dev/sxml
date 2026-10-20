import { load } from '@tinyactors/sxml';

// Load the module, which has every language and feature.
const sxml = await load();
const processor = sxml.createProcessor();

const library = processor.parse(`<books>
  <book year="1979"><title>Godel, Escher, Bach</title></book>
  <book year="1968"><title>The Art of Computer Programming</title></book>
  <book year="1965"><title>Dune</title></book>
</books>`);

// Compile once...
const before = processor.compile('//book[@year < $year]/title/string()', {
  variables: ['year'],
});

// ...and evaluate as often as you like.
for (const year of [1970, 1966]) {
  const titles = before.evaluate({ context: library, variables: { year } });
  console.log(`before ${year}:`, titles.toString().split('\n'));
}

// Results serialize by any method; JavaScript data becomes maps and arrays.
const summary = processor.evaluate(
  'array { //book ! { "title": string(title), "year": xs:integer(@year) } }',
  { context: library });
console.log(summary.serialize({ method: 'json' }));

processor.dispose();
