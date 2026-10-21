// Runs the commands of stages.json in this directory with the sxml
// command and writes what each prints to output/STAGE-INDEX.txt, which
// the landing page shows. SXML names the program (default: the release
// build, ../../build/release/apps/sxml).
//
//   node website/pipeline/run.mjs
import { execFileSync } from 'node:child_process'
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const directory = dirname(fileURLToPath(import.meta.url))
const sxml = resolve(process.env.SXML ?? join(directory, '../../build/release/apps/sxml'))
const { stages } = JSON.parse(readFileSync(join(directory, 'stages.json'), 'utf8'))

rmSync(join(directory, 'output'), { recursive: true, force: true })
mkdirSync(join(directory, 'output'))
for (const stage of stages) {
  stage.commands.forEach((command, index) => {
    const script = command.run.replace(/^sxml /, `"${sxml}" `)
    let printed
    let status = 0
    try {
      printed = execFileSync('sh', ['-c', `${script} 2>&1`], { cwd: directory, encoding: 'utf8' })
    } catch (failure) {
      printed = failure.stdout
      status = failure.status
    }
    if (status !== (command.status ?? 0)) {
      throw new Error(`${stage.id} ${index}: exit ${status}, expected ${command.status ?? 0}\n${printed}`)
    }
    writeFileSync(join(directory, 'output', `${stage.id}-${index}.txt`), printed)
  })
}
rmSync(join(directory, 'bad.xml'))
