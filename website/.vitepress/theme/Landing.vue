<script setup lang="ts">
import Pipeline from './Pipeline.vue'

// The standards, design rules, streaming figures and conformance counts
// are the repository's README.md (What it does, Design, Examples,
// Conformance); keep them in step.

const standards = [
  { area: 'Parsing', text: 'XML 1.0 (Fifth Edition) and XML 1.1, Namespaces in XML 1.0 and 1.1, from UTF-8, UTF-16, ISO-8859-1 and US-ASCII' },
  { area: 'DTDs', text: 'Internal and external subsets, external entities, attribute defaults, DTD validation' },
  { area: 'HTML', text: 'Parsing as the HTML Living Standard specifies it, in every encoding of the Encoding Standard' },
  { area: 'Expressions', text: 'XPath 4.0 and the F&O 4.0 function library: maps, arrays, records, higher-order functions, regular expressions, dates and times, JSON and CSV' },
  { area: 'Queries', text: 'XQuery 4.0, the XQuery Update Facility 3.0, XQuery and XPath Full Text 3.0' },
  { area: 'Transformations', text: 'XSLT 4.0 with streaming, packages, accumulators and xsl:evaluate; XSLT 1.0, 2.0 and 3.0 stylesheets; EXSLT Common' },
  { area: 'Schemas', text: 'XSD 1.1 validation; schema-aware XPath, XQuery and XSLT' },
  { area: 'Output', text: 'Serialization 4.0: xml, xhtml, html, text, json, csv and adaptive' },
  { area: 'Grammars', text: 'Invisible XML 1.0' },
  { area: 'Extensions', text: 'EXPath Binary 4.0 and EXPath File 4.0' },
  { area: 'Text', text: 'Unicode 16.0, the Unicode Collation Algorithm with CLDR tailorings, the IANA Time Zone Database' },
]

const design = [
  { title: 'Portable C17', text: 'One code base. It depends on nothing outside the repository and builds with Clang, GCC and Zig.' },
  { title: 'Caller-provided memory', text: 'The library never calls malloc. Every object takes an sxml_allocator, or goes in memory you provide after a memory_requirements query.' },
  { title: 'No recursion', text: 'The parsers and the evaluator use explicit stacks. Deeply nested input does not overflow the C stack; depth and work have limits you configure.' },
  { title: 'One evaluator', text: 'XPath, XQuery and XSLT compile to one intermediate representation, run on one evaluator and share one data model, XDM 4.0.' },
  { title: 'No global state', text: 'Separate processors share nothing, so a program can run as many as it needs, on as many threads.' },
  { title: 'No network access', text: 'sxml reads only what your resource resolver gives it. File access through EXPath File is off until a host turns it on.' },
]

const conformance = [
  { suite: 'XPath 4.0 and F&O 4.0', source: 'QT4 · XP40', run: '32,841', passed: '32,815', rate: 99.92 },
  { suite: 'XQuery 4.0, Update and Full Text', source: 'QT4 · XQ40', run: '46,232', passed: '46,158', rate: 99.84 },
  { suite: 'XSLT 4.0', source: 'XT40', run: '12,518', passed: '12,268', rate: 98.0 },
  { suite: 'XSD 1.1', source: 'W3C XSD test suite', run: '41,610', passed: '41,599', rate: 99.97 },
  { suite: 'XML 1.0 and 1.1', source: 'W3C XML Conformance Test Suite', run: '2,237', passed: '2,237', rate: 100 },
  { suite: 'HTML tokenizer', source: 'html5lib', run: '7,045', passed: '7,045', rate: 100 },
  { suite: 'Invisible XML', source: 'Community test suite', run: '886', passed: '886', rate: 100 },
]

const targets = [
  {
    title: 'The sxml command',
    where: 'Linux · macOS · Windows · WASI',
    text: 'Subcommands check, validate, xpath, xquery, transform and serialize. Static executables for x86_64 and AArch64, and a WebAssembly program for WASI runtimes.',
    code: 'sxml help',
  },
  {
    title: 'The C library',
    where: 'C17 · static or shared',
    text: 'A processor holds names, types and values. Expressions, queries, stylesheets and schemas compile once and run against any number of documents.',
    code: '#include "sxml/xpath.h"',
  },
  {
    title: 'JavaScript',
    where: 'Browsers · Node.js',
    text: 'The @tinyactors/sxml package: the same library compiled to WebAssembly, with a TypeScript API and no runtime dependencies.',
    code: "import { load } from '@tinyactors/sxml'",
  },
]
</script>

<template>
  <div class="sx">
    <section class="sx-hero">
      <div class="sx-wrap">
        <div class="sx-eyebrow"><span />sxml 0.2.0 · MIT License</div>
        <h1 class="sx-hero__title">An XML toolchain in portable C.</h1>
        <p class="sx-hero__lead">
          sxml parses XML and HTML, validates documents against XSD 1.1 schemas and DTDs,
          and runs XPath 4.0, XQuery 4.0 and XSLT 4.0. It is a C library with no
          dependencies, the <code>sxml</code> command, and a WebAssembly module for JavaScript.
        </p>
        <div class="sx-actions">
          <a class="sx-btn sx-btn--primary" href="/book/" target="_self">Read the book <span aria-hidden="true">→</span></a>
          <a class="sx-btn" href="https://github.com/tinyactors-dev/sxml">Source on GitHub</a>
        </div>
      </div>
      <div class="sx-wrap sx-hero__plate">
        <h2 class="sx-h3">From a server log to a chart</h2>
        <p class="sx-hero__plate-text">
          Nine lines of a web server's access log become XML, are validated and queried,
          summarized by path and drawn as SVG. The files and outputs below are real,
          recorded with the <code>sxml</code> command.
        </p>
        <Pipeline />
      </div>
    </section>

    <section class="sx-section">
      <div class="sx-wrap sx-columns">
        <div>
          <h2 class="sx-h2">Standards</h2>
          <p class="sx-sub">All languages share one data model, XDM 4.0, and every build includes every feature.</p>
        </div>
        <dl class="sx-standards">
          <template v-for="standard in standards" :key="standard.area">
            <dt>{{ standard.area }}</dt>
            <dd>{{ standard.text }}</dd>
          </template>
        </dl>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap">
        <h2 class="sx-h2">Design</h2>
        <div class="sx-traits">
          <article v-for="trait in design" :key="trait.title" class="sx-trait">
            <h3>{{ trait.title }}</h3>
            <p>{{ trait.text }}</p>
          </article>
        </div>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap sx-split">
        <div>
          <h2 class="sx-h2">Streaming</h2>
          <p class="sx-sub">
            In an XSLT mode declared <code>streamable="yes"</code>, <code>sxml transform</code>
            reads its input as it parses it and does not build a tree. Summing one million
            orders in a 76 MB document takes 11 MB of memory; a 974 MB document also takes 11 MB.
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
          <figcaption>Peak memory of <code>sxml transform total.xsl orders.xml</code> on a 76 MB input</figcaption>
        </figure>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap">
        <h2 class="sx-h2">Conformance</h2>
        <p class="sx-sub">Results against the W3C and community test suites, from a release build at commit <code>e74d4ad</code> on 2026-10-20.</p>
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
        <p class="sx-note">Bars start at 95 %. <a href="https://github.com/tinyactors-dev/sxml/blob/main/docs/CONFORMANCE.md">Results by test set</a></p>
      </div>
    </section>

    <section class="sx-section sx-section--rule">
      <div class="sx-wrap">
        <h2 class="sx-h2">Platforms</h2>
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
        <h2 class="sx-h2">The sxml Book</h2>
        <p class="sx-sub">
          A guide to sxml and its languages: installing it, XPath, XQuery, XSLT, XSD, other
          formats, and embedding sxml in C and JavaScript. Each part is taught on a project
          for one made-up feature-flag service, from its specification to its generated SDK clients.
        </p>
        <div class="sx-actions">
          <a class="sx-btn sx-btn--primary" href="/book/" target="_self">Read the book <span aria-hidden="true">→</span></a>
          <a class="sx-btn" href="https://github.com/tinyactors-dev/sxml/blob/main/apps/sxml/README.md">Command reference</a>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.sx{background:var(--bg-app);color:var(--fg-1)}
.sx-wrap{max-width:1120px;margin:0 auto;padding:0 24px}
.sx code{font:400 .88em var(--font-mono);background:var(--surface-2);padding:1px 4px;border-radius:1px}
.sx a:not(.sx-btn){color:var(--fg-1);text-decoration:underline;text-underline-offset:2px}

.sx-eyebrow{display:flex;align-items:center;gap:10px;font:500 11px/1 var(--font-mono);letter-spacing:var(--tracking-label);text-transform:uppercase;color:var(--fg-3)}
.sx-eyebrow>span{width:28px;height:1px;background:var(--fg-3)}
.sx-eyebrow--plain{margin-bottom:12px}

.sx-hero{padding-top:80px;padding-bottom:88px}
.sx-hero__title{font:400 64px/1.02 var(--font-serif);letter-spacing:var(--tracking-display);margin:20px 0 0;max-width:820px;text-wrap:balance}
.sx-hero__lead{font:400 18px/1.55 var(--font-sans);color:var(--fg-2);margin-top:24px;max-width:640px}
.sx-hero__plate{margin-top:72px}
.sx-h3{font:600 18px/1.2 var(--font-sans);margin:0;border:0;padding:0}
.sx-hero__plate-text{font:400 15px/1.55 var(--font-sans);color:var(--fg-2);margin:8px 0 18px;max-width:640px}

.sx-actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:32px}
.sx-btn{display:inline-flex;align-items:center;gap:8px;height:40px;padding:0 18px;border-radius:2px;border:1px solid var(--border-2);font:500 14px/1 var(--font-sans);color:var(--fg-1);background:transparent;text-decoration:none;transition:background 120ms var(--ease-step),border-color 120ms var(--ease-step)}
.sx-btn:hover{border-color:var(--fg-1);background:var(--surface-1)}
.sx-btn--primary{background:var(--accent);border-color:transparent;color:var(--fg-on-accent)}
.sx-btn--primary:hover{background:var(--accent-hover);border-color:transparent}
.sx-btn--primary:active{background:var(--accent-press);transform:translateY(1px)}
.sx-btn:focus-visible{outline:2px solid var(--accent);outline-offset:2px}

.sx-section{padding:88px 0}
.sx-section{border-top:1px solid var(--border-1)}
.sx-h2{font:400 40px/1.1 var(--font-serif);letter-spacing:var(--tracking-display);margin:0;border:0;padding:0}
.sx-sub{font:400 18px/1.6 var(--font-serif);color:var(--fg-2);max-width:680px;margin-top:16px}

.sx-columns{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,2fr);gap:56px}
.sx-standards{display:grid;grid-template-columns:140px minmax(0,1fr);margin:0;border-top:1px solid var(--rule)}
.sx-standards dt,.sx-standards dd{margin:0;padding:12px 0;border-bottom:1px solid var(--border-1)}
.sx-standards dt{font:500 11px/1.9 var(--font-mono);letter-spacing:var(--tracking-label);text-transform:uppercase;color:var(--fg-3)}
.sx-standards dd{font:400 15px/1.5 var(--font-sans)}

.sx-traits{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));margin-top:40px;border-top:1px solid var(--border-1);border-left:1px solid var(--border-1)}
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

.sx-targets{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:24px;margin-top:40px}
.sx-target{padding:24px;border:1px solid var(--border-1);border-radius:2px;background:var(--surface-1);display:flex;flex-direction:column}
.sx-target p{flex:1}
.sx .sx-target__code{display:block;margin-top:18px;padding:10px 12px;font-size:12px;background:var(--surface-code);border:1px solid var(--border-1);color:var(--fg-1);overflow-x:auto;white-space:nowrap}

.sx-cta{padding-bottom:112px}

@media (max-width:960px){
  .sx-split,.sx-columns{grid-template-columns:minmax(0,1fr);gap:32px}
  .sx-traits{grid-template-columns:repeat(2,minmax(0,1fr))}
  .sx-targets{grid-template-columns:minmax(0,1fr)}
}
@media (max-width:640px){
  .sx-wrap{padding:0 16px}
  .sx-hero{padding-top:48px;padding-bottom:56px}
  .sx-hero__title{font-size:44px}
  .sx-hero__plate{margin-top:48px}
  .sx-h2{font-size:32px}
  .sx-section{padding:64px 0}
  .sx-traits{grid-template-columns:minmax(0,1fr)}
  .sx-standards{grid-template-columns:minmax(0,1fr)}
  .sx-standards dt{border-bottom:0;padding-bottom:0}
  .sx-table__source{display:none}
  .sx-table th:nth-child(2),.sx-table td:nth-child(2){display:none}
  .sx-rate{display:none}
  .sx-table__rate{width:auto}
}
</style>
