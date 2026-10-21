<script setup lang="ts">
import { ref } from 'vue'

// Every example and number on this page comes from the repository's
// README.md (Quick start, Examples, Conformance, Design); keep them in step.
const examples = [
  {
    name: 'XPath',
    lines: [
      { prompt: true, text: `sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'` },
      { text: '"Godel, Escher, Bach"' },
      { text: '"The Art of Computer Programming"' },
      { prompt: true, text: `sxml xpath --method=json 'map { "a": [1, 2] }'` },
      { text: '{"a":[1,2]}' },
    ],
  },
  {
    name: 'XQuery',
    lines: [
      { prompt: true, text: `sxml xquery --context-item=reviews.xml --method=adaptive \\` },
      { text: `    --expression='//review[. contains text "parse" using stemming]/@id/string()'`, input: true },
      { text: '"2"' },
    ],
  },
  {
    name: 'XSLT',
    lines: [
      { prompt: true, text: 'sxml transform total.xsl orders.xml' },
      { text: '499,995,000.00' },
      { comment: true, text: '# 76 MB of orders, streamed in 11 MB of memory' },
    ],
  },
  {
    name: 'XSD',
    lines: [
      { prompt: true, text: 'sxml validate --schema=books.xsd books.xml' },
      { prompt: true, text: `echo '<books><book year="soon"><title>Dune</title></book></books>' \\` },
      { text: '    | sxml validate --schema=books.xsd -', input: true },
      { text: "-:1:14: cvc-datatype-valid.1.2.1: attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'" },
      { text: "-:1:8: cvc-complex-type.2.4: the content of element 'book' is incomplete; expected 'price'" },
    ],
  },
]
const selected = ref(0)

const standards = [
  'XML 1.0 & 1.1', 'Namespaces', 'DTDs', 'HTML Living Standard', 'XDM 4.0', 'XPath 4.0',
  'F&O 4.0', 'XQuery 4.0', 'XQuery Update 3.0', 'Full Text 3.0', 'XSLT 4.0', 'XSD 1.1',
  'Serialization 4.0', 'Invisible XML 1.0', 'EXPath Binary & File', 'Unicode 16.0',
]

const traits = [
  {
    title: 'Portable C17',
    text: 'One code base with no dependencies outside the repository. Built and tested with Clang and GCC; the command cross-compiles for Linux, macOS, Windows and WASI.',
  },
  {
    title: 'Your memory',
    text: 'sxml never calls malloc itself. Every object takes an sxml_allocator, or is placed in memory you provide after a memory_requirements query.',
  },
  {
    title: 'No recursion',
    text: 'Parsers and the evaluator use explicit stacks, so deeply nested documents and queries do not overflow the C stack. Depth and work are bounded by limits you set.',
  },
  {
    title: 'One engine',
    text: 'XPath, XQuery and XSLT compile to one intermediate representation, run on one evaluator and share one data model, XDM 4.0.',
  },
  {
    title: 'No global state',
    text: 'Every object lives in memory you control, and separate processors share nothing. Compile once, evaluate against any number of documents.',
  },
  {
    title: 'No network access',
    text: 'sxml reads only what your resource resolver gives it. File access through EXPath File is a feature a host names on purpose.',
  },
]

const conformance = [
  { suite: 'XPath 4.0 and F&O 4.0', source: 'QT4 · XP40', run: '32,841', passed: '32,815', rate: 99.92 },
  { suite: 'XQuery 4.0, Update and Full Text', source: 'QT4 · XQ40', run: '46,232', passed: '46,158', rate: 99.84 },
  { suite: 'XSLT 4.0', source: 'XT40', run: '12,518', passed: '12,268', rate: 98.0 },
  { suite: 'XSD 1.1', source: 'W3C XSD test suite', run: '41,610', passed: '41,599', rate: 99.97 },
  { suite: 'XML 1.0 and 1.1', source: 'W3C XML Conformance', run: '2,237', passed: '2,237', rate: 100 },
  { suite: 'HTML tokenizer', source: 'html5lib', run: '7,045', passed: '7,045', rate: 100 },
  { suite: 'Invisible XML', source: 'Community test suite', run: '886', passed: '886', rate: 100 },
]

const targets = [
  {
    title: 'The sxml command',
    where: 'Linux · macOS · Windows · WASI',
    text: 'Check, validate, query and transform from the shell. Static executables for x86_64 and AArch64, and a WebAssembly program for any WASI runtime.',
    code: 'sxml help',
  },
  {
    title: 'The C library',
    where: 'C17 · static or shared',
    text: 'Add the repository with add_subdirectory and link sxml. A processor holds names, types and values; expressions compile once and run many times.',
    code: '#include "sxml/xpath.h"',
  },
  {
    title: 'JavaScript',
    where: 'Browsers · Node.js',
    text: 'The @tinyactors/sxml package: a TypeScript wrapper over the same library as WebAssembly, with every language and no runtime dependencies.',
    code: "import { load } from '@tinyactors/sxml'",
  },
]
</script>

<template>
  <div class="sx">
    <section class="sx-hero">
      <div class="sx-wrap sx-hero__grid">
        <div>
          <div class="sx-eyebrow"><span />XML toolchain in portable C</div>
          <h1 class="sx-hero__title">The XML stack,<br />as one <em>small</em> library.</h1>
          <p class="sx-hero__lead">
            sxml parses XML and HTML, validates against XSD 1.1 and DTDs, and runs
            XPath 4.0, XQuery 4.0 and XSLT 4.0. No dependencies. Use it from C,
            from JavaScript in browsers and Node.js, or as the <code>sxml</code> command.
          </p>
          <div class="sx-actions">
            <a class="sx-btn sx-btn--primary" href="/book/" target="_self">Read the book <span aria-hidden="true">→</span></a>
            <a class="sx-btn" href="https://github.com/tinyactors-dev/sxml">Source on GitHub</a>
          </div>
          <p class="sx-hero__status">v0.2.0 · in active development · MIT License</p>
        </div>

        <div class="sx-sheet">
          <span class="sx-reg sx-reg--tl"><i /></span><span class="sx-reg sx-reg--tr"><i /></span>
          <span class="sx-reg sx-reg--bl"><i /></span><span class="sx-reg sx-reg--br"><i /></span>
          <div class="sx-sheet__frame">
            <div class="sx-term">
              <div class="sx-tabs" role="tablist">
                <button
                  v-for="(example, index) in examples"
                  :key="example.name"
                  role="tab"
                  :aria-selected="index === selected"
                  :class="{ 'is-active': index === selected }"
                  @click="selected = index"
                >{{ example.name }}</button>
              </div>
              <pre class="sx-term__body"><template
                v-for="(line, index) in examples[selected].lines"
                :key="index"
              ><span v-if="line.prompt" class="sx-term__prompt">$ </span><span
                :class="{ 'sx-term__out': !line.prompt && !line.input, 'sx-term__comment': line.comment }"
              >{{ line.text }}</span>
</template></pre>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="sx-standards">
      <div class="sx-wrap">
        <ul>
          <li v-for="standard in standards" :key="standard">{{ standard }}</li>
        </ul>
      </div>
    </section>

    <section class="sx-section">
      <div class="sx-wrap">
        <h2 class="sx-h2">Small by design</h2>
        <p class="sx-sub">
          A small set of orthogonal mechanisms: one data model, one evaluator, and thin
          layers for each language. New features compose what is there.
        </p>
        <div class="sx-traits">
          <article v-for="trait in traits" :key="trait.title" class="sx-trait">
            <h3>{{ trait.title }}</h3>
            <p>{{ trait.text }}</p>
          </article>
        </div>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap sx-split">
        <div>
          <h2 class="sx-h2">Streams what does not fit</h2>
          <p class="sx-sub">
            In an XSLT mode declared <code>streamable="yes"</code>, sxml reads its input
            as it parses it and never builds a tree. A 76 MB document of one million
            orders is summed in 11 MB; a 974 MB document streams in 11 MB too.
          </p>
        </div>
        <figure class="sx-mem" aria-label="Peak memory to sum 76 MB of orders">
          <div class="sx-mem__row">
            <span class="sx-mem__label">streamed</span>
            <span class="sx-mem__track"><span class="sx-mem__bar sx-mem__bar--hot" style="width: 1.84%" /></span>
            <span class="sx-mem__value">11 MB</span>
          </div>
          <div class="sx-mem__row">
            <span class="sx-mem__label">tree</span>
            <span class="sx-mem__track"><span class="sx-mem__bar" style="width: 100%" /></span>
            <span class="sx-mem__value">597 MB</span>
          </div>
          <figcaption>Peak memory of <code>sxml transform total.xsl orders.xml</code>, 76 MB input</figcaption>
        </figure>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap">
        <h2 class="sx-h2">Measured against the W3C test suites</h2>
        <p class="sx-sub">Counts of a release build at commit <code>e74d4ad</code>, 2026-10-20.</p>
        <table class="sx-table">
          <thead>
            <tr><th>Suite</th><th class="num">Run</th><th class="num">Passed</th><th class="sx-table__rate">Rate</th></tr>
          </thead>
          <tbody>
            <tr v-for="row in conformance" :key="row.suite">
              <td>{{ row.suite }}<span class="sx-table__source">{{ row.source }}</span></td>
              <td class="num">{{ row.run }}</td>
              <td class="num">{{ row.passed }}</td>
              <td class="sx-table__rate">
                <div class="sx-table__ratecell">
                  <span class="sx-rate"><span :style="{ width: `${(row.rate - 95) * 20}%` }" /></span>
                  <span class="num">{{ row.rate.toFixed(2) }} %</span>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
        <p class="sx-note">Bars start at 95 %.</p>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap">
        <h2 class="sx-h2">Where it runs</h2>
        <div class="sx-targets">
          <article v-for="target in targets" :key="target.title" class="sx-target">
            <div class="sx-eyebrow sx-eyebrow--plain">{{ target.where }}</div>
            <h3>{{ target.title }}</h3>
            <p>{{ target.text }}</p>
            <code class="sx-target__code">{{ target.code }}</code>
          </article>
        </div>
      </div>
    </section>

    <section class="sx-section sx-section--rule sx-cta">
      <div class="sx-wrap">
        <h2 class="sx-h2">Learn it on real projects</h2>
        <p class="sx-sub">
          The sxml Book teaches XPath, XQuery, XSLT and XSD step by step, from installing
          sxml to embedding it in C and JavaScript, on projects around one feature-flag
          service: its specification, SVG diagrams, an SCXML machine, call-chain and
          trace diffs, and SDK clients generated from a schema.
        </p>
        <div class="sx-actions">
          <a class="sx-btn sx-btn--primary" href="/book/" target="_self">Read the book <span aria-hidden="true">→</span></a>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.sx{background:var(--bg-app);color:var(--fg-1)}
.sx-wrap{max-width:1120px;margin:0 auto;padding:0 24px}
.sx code{font:400 .88em var(--font-mono);background:var(--surface-2);padding:1px 4px;border-radius:1px}

.sx-eyebrow{display:flex;align-items:center;gap:10px;font:500 11px/1 var(--font-mono);letter-spacing:var(--tracking-label);text-transform:uppercase;color:var(--fg-3)}
.sx-eyebrow>span{width:28px;height:1px;background:var(--fg-3)}
.sx-eyebrow--plain{margin-bottom:12px}

.sx-hero{border-bottom:1px solid var(--border-1)}
.sx-hero__grid{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.05fr);gap:56px;align-items:center;padding-top:88px;padding-bottom:88px}
.sx-hero__title{font:400 72px/1 var(--font-serif);letter-spacing:var(--tracking-display);margin:20px 0 0}
.sx-hero__title em{font-style:italic}
.sx-hero__lead{font:400 18px/1.55 var(--font-sans);color:var(--fg-2);margin-top:24px;max-width:500px}
.sx-hero__status{margin-top:28px;font:400 13px var(--font-mono);color:var(--fg-3)}

.sx-actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:32px}
.sx-btn{display:inline-flex;align-items:center;gap:8px;height:40px;padding:0 18px;border-radius:2px;border:1px solid var(--border-2);font:500 14px/1 var(--font-sans);color:var(--fg-1);background:transparent;text-decoration:none;transition:background 120ms var(--ease-step),border-color 120ms var(--ease-step)}
.sx-btn:hover{border-color:var(--fg-1);background:var(--surface-1)}
.sx-btn--primary{background:var(--accent);border-color:transparent;color:var(--fg-on-accent)}
.sx-btn--primary:hover{background:var(--accent-hover);border-color:transparent}
.sx-btn--primary:active{background:var(--accent-press);transform:translateY(1px)}
.sx-btn:focus-visible,.sx-tabs button:focus-visible{outline:2px solid var(--accent);outline-offset:2px}

/* The Sheet: a ruled frame, an inset hairline, registration marks. */
.sx-sheet{position:relative;padding:14px}
.sx-sheet__frame{position:relative;border:1.5px solid var(--rule);background:var(--bg-grid),var(--surface-1);padding:20px}
.sx-sheet__frame::before{content:"";position:absolute;inset:4px;border:.5px solid var(--border-2);pointer-events:none}
.sx-reg{position:absolute;width:14px;height:14px}
.sx-reg::before,.sx-reg::after{content:"";position:absolute;background:var(--rule)}
.sx-reg::before{left:6.5px;top:0;width:1px;height:14px}
.sx-reg::after{top:6.5px;left:0;height:1px;width:14px}
.sx-reg i{position:absolute;inset:3px;border:1px solid var(--rule);border-radius:50%}
.sx-reg--tl{left:-7px;top:-7px}.sx-reg--tr{right:-7px;top:-7px}.sx-reg--bl{left:-7px;bottom:-7px}.sx-reg--br{right:-7px;bottom:-7px}

.sx-term{position:relative;background:var(--surface-code);border:1px solid var(--border-1);border-radius:2px}
.sx-tabs{display:flex;border-bottom:1px solid var(--border-1)}
.sx-tabs button{font:500 12px/1 var(--font-mono);color:var(--fg-3);padding:11px 14px;border-bottom:1.5px solid transparent;margin-bottom:-1px;transition:color 120ms var(--ease-step)}
.sx-tabs button:hover{color:var(--fg-1)}
.sx-tabs button.is-active{color:var(--fg-1);border-bottom-color:var(--accent)}
.sx-term__body{margin:0;padding:16px 16px 18px;min-height:176px;font:400 13px/1.6 var(--font-mono);color:var(--code-fg);white-space:pre-wrap;word-break:break-word}
.sx-term__prompt{color:var(--accent-fg);user-select:none}
.sx-term__out{color:var(--fg-2)}
.sx-term__comment{color:var(--code-muted)}

.sx-standards{border-bottom:1px solid var(--border-1);background:var(--surface-1)}
.sx-standards ul{list-style:none;margin:0;padding:22px 0;display:flex;flex-wrap:wrap;gap:8px 22px;font:400 13px/1.4 var(--font-mono);color:var(--fg-2)}

.sx-section{padding:88px 0}
.sx-section--rule{border-top:1px solid var(--border-1)}
.sx-h2{font:400 40px/1.1 var(--font-serif);letter-spacing:var(--tracking-display);margin:0;border:0;padding:0}
.sx-sub{font:400 18px/1.6 var(--font-serif);color:var(--fg-2);max-width:680px;margin-top:16px}

.sx-traits{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));margin-top:48px;border-top:1px solid var(--border-1);border-left:1px solid var(--border-1)}
.sx-trait{padding:28px 28px 32px;border-right:1px solid var(--border-1);border-bottom:1px solid var(--border-1);background:var(--surface-1)}
.sx-trait h3,.sx-target h3{font:600 18px/1.2 var(--font-sans);margin:0}
.sx-trait p,.sx-target p{font:400 15px/1.55 var(--font-sans);color:var(--fg-2);margin-top:10px}

.sx-split{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:56px;align-items:center}
.sx-mem{margin:0;display:flex;flex-direction:column;gap:14px}
.sx-mem__row{display:grid;grid-template-columns:72px 1fr 64px;gap:12px;align-items:center}
.sx-mem__label{font:400 12px var(--font-mono);color:var(--fg-3)}
.sx-mem__track{height:22px;border:1px solid var(--border-2);background:var(--bg-grid),var(--surface-1)}
.sx-mem__bar{display:block;height:100%;background:var(--fg-4);min-width:3px}
.sx-mem__bar--hot{background:var(--accent)}
.sx-mem__value{font:500 13px var(--font-mono);text-align:right}
.sx-mem figcaption{font:400 12px var(--font-sans);color:var(--fg-3);margin-top:4px}

.sx-table{display:table;width:100%;margin-top:40px;border-collapse:collapse;font:400 15px/1.4 var(--font-sans)}
.sx-table tr{background:transparent!important;border:0}
.sx-table th,.sx-table td{border:0;border-bottom:1px solid var(--border-1);padding:14px 12px 14px 0;text-align:left;background:transparent}
.sx-table th{font:500 11px/1 var(--font-mono);letter-spacing:var(--tracking-label);text-transform:uppercase;color:var(--fg-3);border-bottom-color:var(--rule)}
.sx-table .num{text-align:right;white-space:nowrap;font-family:var(--font-mono);font-size:13px;font-variant-numeric:tabular-nums}
.sx-table__source{display:block;font:400 12px var(--font-mono);color:var(--fg-3);margin-top:2px}
.sx-table__rate{width:34%;padding-right:0!important}
.sx-table__ratecell{display:flex;align-items:center;gap:14px;justify-content:flex-end}
.sx-rate{flex:1;height:6px;background:var(--surface-3)}
.sx-rate span{display:block;height:100%;background:var(--accent)}
.sx-note{font:400 12px var(--font-sans);color:var(--fg-3);margin-top:12px}

.sx-targets{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:24px;margin-top:48px}
.sx-target{padding:24px;border:1px solid var(--border-1);border-radius:2px;background:var(--surface-1);display:flex;flex-direction:column}
.sx-target p{flex:1}
.sx .sx-target__code{display:block;margin-top:18px;padding:10px 12px;font-size:12px;background:var(--surface-code);border:1px solid var(--border-1);color:var(--fg-1);overflow-x:auto;white-space:nowrap}

.sx-cta{padding-bottom:112px}

@media (max-width:960px){
  .sx-hero__grid,.sx-split{grid-template-columns:minmax(0,1fr);gap:40px}
  .sx-traits{grid-template-columns:repeat(2,minmax(0,1fr))}
  .sx-targets{grid-template-columns:minmax(0,1fr)}
}
@media (max-width:640px){
  .sx-wrap{padding:0 16px}
  .sx-hero__grid{padding-top:48px;padding-bottom:56px}
  .sx-hero__title{font-size:48px}
  .sx-h2{font-size:32px}
  .sx-section{padding:64px 0}
  .sx-traits{grid-template-columns:minmax(0,1fr)}
  .sx-table__source{display:none}
  .sx-table th:nth-child(2),.sx-table td:nth-child(2){display:none}
  .sx-rate{display:none}
  .sx-table__rate{width:auto}
}
</style>
