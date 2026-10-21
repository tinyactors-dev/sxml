import { readFileSync } from 'node:fs'
import { extname, join } from 'node:path'
import { createCssVariablesTheme, createHighlighter, type LanguageRegistration } from 'shiki'
import { defineLoader } from 'vitepress'
import ixml from './grammars/ixml.json'
import xquery from './grammars/xquery.json'

// The pipeline example of the landing page, read from website/pipeline/
// (stages.json, its files and the outputs run.mjs recorded) and
// highlighted at build time.

export interface Code {
  name: string
  html: string
}
export interface Command {
  command: string
  note?: string
  output: string
  wrap: boolean
}
export interface Stage {
  id: string
  name: string
  summary: string
  files: Code[]
  commands: Command[]
}

// The board: language blocks (by stage index), with the files that pass
// between them on the trace.
export type Part = { file: string } | { stage: number }
export interface Pipeline {
  board: Part[]
  stages: Stage[]
}

declare const data: Pipeline
export { data }

interface StageSource {
  id: string
  name: string
  summary: string
  files: (string | { name: string; output: string })[]
  commands: { run: string; show?: string; note?: string; lang?: string; hidden?: boolean }[]
}

const languages: Record<string, string> = {
  '.ixml': 'ixml', '.xml': 'xml', '.xsd': 'xml', '.xsl': 'xml', '.xq': 'xquery', '.txt': 'text',
  '.md': 'markdown', '.ts': 'typescript', '.sh': 'shellscript',
}

// A summary's `code` spans become <code> elements.
const inline = (text: string) =>
  text
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/`([^`]+)`/g, '<code>$1</code>')

export default defineLoader({
  watch: ['pipeline/**'],
  async load(): Promise<Pipeline> {
    const { srcDir } = (globalThis as { VITEPRESS_CONFIG: { srcDir: string } }).VITEPRESS_CONFIG
    const directory = join(srcDir, 'pipeline')
    const read = (name: string) => readFileSync(join(directory, name), 'utf8')
    const highlighter = await createHighlighter({
      themes: [createCssVariablesTheme({ name: 'sxml', variablePrefix: '--code-' })],
      langs: ['xml', 'markdown', 'typescript', 'shellscript', ixml as unknown as LanguageRegistration, xquery as unknown as LanguageRegistration],
    })
    const highlight = (code: string, lang: string) =>
      highlighter.codeToHtml(code.replace(/\n$/, ''), { lang, theme: 'sxml' })

    const source = JSON.parse(read('stages.json')) as {
      board: ({ file: string } | { stage: string })[]
      stages: StageSource[]
    }
    const stages = source.stages.map((stage) => ({
      id: stage.id,
      name: stage.name,
      summary: inline(stage.summary),
      files: stage.files.map((file) =>
        typeof file === 'string'
          ? { name: file, html: highlight(read(file), languages[extname(file)] ?? 'text') }
          : { name: file.name, html: highlight(read(`output/${file.output}.txt`), languages[extname(file.name)] ?? 'text') },
      ),
      commands: stage.commands.flatMap((command, index) => {
        if (command.hidden) return []
        const output = read(`output/${stage.id}-${index}.txt`)
        return [{
          command: command.show ?? command.run,
          note: command.note,
          wrap: !command.lang,
          output: output ? highlight(output, command.lang ?? 'text') : '',
        }]
      }),
    }))
    const board = source.board.map((part) =>
      'file' in part ? part : { stage: source.stages.findIndex((stage) => stage.id === part.stage) },
    )
    return { board, stages }
  },
})
