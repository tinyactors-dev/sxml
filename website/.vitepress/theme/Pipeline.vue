<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import { data as stages } from './pipeline.data'

// One access log travels through every language: text, Invisible XML,
// XML, XSD, XPath, XQuery, XSLT, SVG. The files and outputs are
// website/pipeline/'s, recorded by its run.mjs.

const selected = ref(0)
const stage = computed(() => stages[selected.value])

// The board: language blocks, with the files that pass between them on
// the trace. A block's index is its stage's.
type Part = { kind: 'file'; name: string } | { kind: 'block'; stage: number }
const parts: Part[] = [
  { kind: 'file', name: 'access.log' },
  { kind: 'block', stage: 0 },
  { kind: 'file', name: 'requests.xml' },
  { kind: 'block', stage: 1 },
  { kind: 'block', stage: 2 },
  { kind: 'block', stage: 3 },
  { kind: 'file', name: 'paths.xml' },
  { kind: 'block', stage: 4 },
  { kind: 'file', name: 'chart.svg' },
]
const reached = (index: number) => {
  const active = parts.findIndex((part) => part.kind === 'block' && part.stage === selected.value)
  return index <= active
}
// On a narrow screen the board scrolls; keep the active block in view.
const board = ref<HTMLElement>()
watch(selected, async () => {
  await nextTick()
  board.value?.querySelector<HTMLElement>('.is-active')?.scrollIntoView({ block: 'nearest', inline: 'center', behavior: 'smooth' })
})
const next = () => {
  selected.value = (selected.value + 1) % stages.length
}
</script>

<template>
  <div class="px-sheet">
    <span class="px-reg px-reg--tl"><i /></span><span class="px-reg px-reg--tr"><i /></span>
    <span class="px-reg px-reg--bl"><i /></span><span class="px-reg px-reg--br"><i /></span>
    <div class="px-frame">
      <div ref="board" class="px-board" role="tablist" aria-label="Pipeline stages">
        <template v-for="(part, index) in parts" :key="index">
          <span v-if="index > 0" class="px-trace" :class="{ 'is-hot': reached(index) }" />
          <span v-if="part.kind === 'file'" class="px-file" :class="{ 'is-hot': reached(index) }">{{ part.name }}</span>
          <button
            v-else
            role="tab"
            class="px-block"
            :class="{ 'is-active': part.stage === selected, 'is-hot': reached(index) }"
            :aria-selected="part.stage === selected"
            @click="selected = part.stage"
          >{{ stages[part.stage].name }}</button>
        </template>
      </div>

      <p class="px-summary" v-html="stage.summary" />

      <div class="px-panes">
        <div class="px-files">
          <figure v-for="file in stage.files" :key="file.name" class="px-code">
            <figcaption>{{ file.name }}</figcaption>
            <div v-html="file.html" />
          </figure>
        </div>
        <div class="px-run">
          <div class="px-code px-terminal">
            <div v-for="(command, index) in stage.commands" :key="index" class="px-step">
              <pre class="px-command"><span class="px-prompt" aria-hidden="true">$ </span>{{ command.command }}</pre>
              <pre v-if="command.note" class="px-note"># {{ command.note }}</pre>
              <div v-if="command.output" class="px-output" :class="{ 'px-output--wrap': command.wrap }" v-html="command.output" />
            </div>
          </div>
          <figure v-for="(command, index) in stage.commands.filter((command) => command.rendered)" :key="`rendered-${index}`" class="px-rendered">
            <figcaption>chart.svg in a browser</figcaption>
            <div class="px-chart" v-html="command.rendered" />
          </figure>
          <button class="px-next" @click="next">
            <template v-if="selected < stages.length - 1">Next: {{ stages[selected + 1].name }} <span aria-hidden="true">→</span></template>
            <template v-else>Back to the text <span aria-hidden="true">↺</span></template>
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* The Sheet: a ruled frame, an inset hairline, registration marks. */
.px-sheet{position:relative;padding:14px}
.px-frame{position:relative;border:1.5px solid var(--rule);background:var(--bg-grid),var(--surface-1);padding:28px}
.px-frame::before{content:"";position:absolute;inset:4px;border:.5px solid var(--border-2);pointer-events:none}
.px-reg{position:absolute;width:14px;height:14px}
.px-reg::before,.px-reg::after{content:"";position:absolute;background:var(--rule)}
.px-reg::before{left:6.5px;top:0;width:1px;height:14px}
.px-reg::after{top:6.5px;left:0;height:1px;width:14px}
.px-reg i{position:absolute;inset:3px;border:1px solid var(--rule);border-radius:50%}
.px-reg--tl{left:-7px;top:-7px}.px-reg--tr{right:-7px;top:-7px}.px-reg--bl{left:-7px;bottom:-7px}.px-reg--br{right:-7px;bottom:-7px}

/* The board: blocks are chips with pin pads, files are hatched pads on
   the trace; the trace is pine as far as the log has travelled. */
.px-board{position:relative;display:flex;align-items:center;overflow-x:auto;padding:6px 2px 10px;scrollbar-width:thin}
.px-trace{flex:1 0 14px;height:1.5px;background:var(--fg-4);transition:background 360ms var(--ease-step)}
.px-trace.is-hot{background:var(--accent);height:2.5px}
.px-block{position:relative;flex:none;padding:0 14px;height:40px;border:1.25px solid var(--rule);border-radius:0;background:var(--surface-1);font:500 13px/1 var(--font-sans);color:var(--fg-1);cursor:pointer;transition:background 140ms var(--ease-step),border-color 140ms var(--ease-step),color 140ms var(--ease-step)}
.px-block::before,.px-block::after{content:"";position:absolute;top:50%;width:5px;height:7px;margin-top:-3.5px;background:var(--rule)}
.px-block::before{left:-5.5px}.px-block::after{right:-5.5px}
.px-block:hover{background:var(--surface-2)}
.px-block.is-active{background:var(--accent-subtle);border-color:var(--accent);color:var(--accent-fg)}
.px-block.is-active::before,.px-block.is-active::after{background:var(--accent)}
.px-block:focus-visible,.px-next:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
.px-file{flex:none;padding:5px 8px;font:400 11px/1 var(--font-mono);color:var(--fg-2);border:1px solid var(--border-2);background:repeating-linear-gradient(135deg,var(--grid-major) 0 1px,transparent 1px 5px),var(--surface-1);transition:border-color 360ms var(--ease-step),color 360ms var(--ease-step)}
.px-file.is-hot{border-color:var(--accent);color:var(--fg-1)}

.px-summary :deep(code){font:400 .85em var(--font-mono);background:var(--surface-2);padding:1px 4px;border-radius:1px}
.px-summary{font:400 18px/1.6 var(--font-serif);color:var(--fg-1);max-width:680px;margin:20px 0 24px;min-height:3.2em}

.px-panes{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:20px;align-items:start}
.px-files{display:flex;flex-direction:column;gap:16px;min-width:0}
.px-run{display:flex;flex-direction:column;gap:16px;min-width:0}
.px-code{margin:0;background:var(--surface-code);border:1px solid var(--border-1);border-radius:2px;min-width:0}
.px-code figcaption{font:400 12px/1 var(--font-mono);color:var(--fg-3);padding:10px 14px;border-bottom:1px solid var(--border-1)}
.px-code :deep(pre){margin:0;padding:14px;overflow-x:auto;font:400 13px/1.6 var(--font-mono);background:transparent!important}
.px-code :deep(code){font:inherit;background:none;padding:0}
.px-terminal{padding:4px 0}
.px-step+.px-step{border-top:1px dashed var(--border-1)}
.px-command,.px-note{margin:0;padding:12px 14px 0;font:400 13px/1.6 var(--font-mono);color:var(--code-fg);white-space:pre-wrap;word-break:break-word}
.px-step>:last-child{padding-bottom:12px}
.px-output :deep(pre){padding:4px 14px 12px}
.px-output--wrap :deep(pre){white-space:pre-wrap;word-break:break-word}
.px-prompt{color:var(--accent-fg);user-select:none}
.px-note{color:var(--code-muted);padding-top:2px}

.px-rendered{margin:0;border:1px solid var(--border-1);background:var(--surface-1);border-radius:2px}
.px-rendered figcaption{font:400 12px/1 var(--font-mono);color:var(--fg-3);padding:10px 14px;border-bottom:1px solid var(--border-1)}
.px-chart{padding:16px 18px 14px}
.px-chart :deep(svg){display:block;width:100%;height:auto;font:400 13px var(--font-mono);fill:var(--fg-1)}
.px-chart :deep(.requests){fill:var(--fg-4)}
.px-chart :deep(.errors){fill:var(--state-error)}

.px-next{align-self:flex-end;display:inline-flex;align-items:center;gap:8px;height:32px;padding:0 14px;border:1px solid var(--border-2);border-radius:2px;font:500 13px/1 var(--font-sans);color:var(--fg-1);background:var(--surface-1);transition:border-color 120ms var(--ease-step)}
.px-next:hover{border-color:var(--fg-1)}

@media (max-width:960px){
  .px-panes{grid-template-columns:minmax(0,1fr)}
}
@media (max-width:640px){
  .px-sheet{padding:10px}
  .px-frame{padding:18px 14px}
  .px-summary :deep(code){font:400 .85em var(--font-mono);background:var(--surface-2);padding:1px 4px;border-radius:1px}
.px-summary{font-size:17px;min-height:0}
}
@media (prefers-reduced-motion:reduce){
  .px-trace,.px-file,.px-block{transition:none}
}
</style>
