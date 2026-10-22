import { readFile } from 'node:fs/promises';
import { load } from '@tinyactors/sxml';

const sxml = await load();
const wasm = await readFile('tree-sitter-json.wasm');

// Link the language file into sxml.wasm, with a highlights query of our own.
const json = await sxml.loadCodeLanguage({
  name: 'my-json',
  wasm,
  highlights: '(number) @number\n[(true) (false)] @boolean\n',
});
console.log(json.name, json.abiVersion);

// A processor knows the languages it is given, beside the built-in ones.
const processor = sxml.createProcessor({ codeLanguages: [json] });
const tree = processor.evaluate('tree-sitter:parse($code, "my-json")', {
  variables: { code: '[1, true]' },
});
console.log(tree.serialize({ method: 'xml' }));

// A query that does not fit the grammar is an error.
try {
  await sxml.loadCodeLanguage({ wasm, highlights: '(float) @number' });
} catch (error) {
  console.log(error.message);
}

processor.dispose();
