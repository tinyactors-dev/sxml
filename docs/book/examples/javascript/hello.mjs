import { readFileSync } from 'node:fs';
import { load } from '@tinyactors/sxml';

// Load the module, which has every language and feature.
const sxml = await load();
const processor = sxml.createProcessor();

const draft = processor.parse(readFileSync('latch.xml', 'utf8'));

// Compile once...
const anchors = processor.compile('//t[bcp14 = $keyword]/@anchor ! string()', {
  variables: ['keyword'],
});

// ...and evaluate as often as you like.
for (const keyword of ['MUST', 'MAY']) {
  const found = anchors.evaluate({ context: draft, variables: { keyword } });
  console.log(`${keyword}:`, found.toString().split('\n'));
}

// Results serialize by any method; maps and arrays become JSON.
const summary = processor.evaluate(
  'array { for $keyword in distinct-values(//bcp14) return { "keyword": $keyword, "count": count(//bcp14[. = $keyword]) } }',
  { context: draft });
console.log(summary.serialize({ method: 'json' }));

processor.dispose();
