# Printing the Tree with XSLT

<!-- examples: codegen -->

The tree says what the program is. The printer says how it is written:
which keywords and punctuation each kind of element becomes, where the
lines break and how far they are indented. This is the kind of job
XSLT's template rules were made for, as in [Transforming with
XSLT](xslt.md): the output follows the input, one rule for each kind of
element, and the processor finds the rule for each one.

## The printer

`print.xsl`:

```xml
{{#include ../examples/codegen/print.xsl}}
```

The stylesheet writes text, so `xsl:output` says `method="text"`, and
the text in a template body is the TypeScript itself: the rule for
`return` reads `return <xsl:apply-templates/>;`. `xsl:strip-space`
drops the spaces and line breaks that indent the tree, except inside
the elements whose text is part of the program, such as `string`.

Let's read the rules in groups.

- *Layout.* Only the first group knows about lines. `print:indent`
  counts the blocks around a node (a function's `body`, a `then`, an
  interface, an object) and indents by `$indent` for each.
  `print:braces` says which blocks get braces: all but the `then` of an
  `if` with one statement and no `else`, which stays on the line of the
  `if`. The rule with priority 1 matches everything that is a line of
  its own: a declaration in a module, a statement in a block, a member
  of an interface, an entry of an object. It writes the indentation,
  calls `xsl:next-match` to let the rule for the element write the
  element, and ends the line. The rule with priority 2 puts an empty
  line before each declaration, and then lets the line rule do the
  rest. So every other rule writes one construct without a thought for
  where it is.
- *Files.* Each `module` of a `program` becomes a file through
  `xsl:result-document`, named by its `file` attribute. A `module` on
  its own is written to standard output, which is what the pieces of
  the last section used.
- *Declarations, types, statements.* One rule each, in the words of the
  vocabulary. Lists are joined with the `separator` attribute of
  `xsl:for-each`, new in XSLT 4.0: parameters, arguments and type
  arguments with `", "`, a union with `" | "`.
- *Expressions.* A `string` becomes a JSON string, which is a valid
  JavaScript string, with every escape it needs; the same function
  writes the `from` of an import. A `regex` escapes its slashes. The
  operators are a map from their names to their symbols, used as the
  separator of a `binary` expression's operands. The generator never
  puts one binary expression into another, so the printer needs no
  rules for parentheses; a printer for any tree would add them where
  the precedence of the operators needs them.
- *Anything else* is an error: the last rule matches every element
  that no other rule does, with the lowest priority, and stops the
  transformation, so an element the printer doesn't know can't be
  dropped without a word:

```console
$ echo '<module><while/></module>' | sxml transform print.xsl -
print.xsl:302:20: print:unknown: The printer has no rule for while
```

## Writing the files

`generate.sh`, in the last section, keeps the tree of the program in
`generated/program.xml`, next to the files printed from it. Print it
into a new directory: the `--result-directory` option lets the
transformation write its result documents there, as in [Many Outputs,
Functions and Packages](xslt-packages.md):

```console
$ mkdir out
$ sxml transform --result-directory=out print.xsl generated/program.xml
$ ls out
browser.ts
console.ts
node.ts
server.ts
types.ts
```

A client module has one async function for each remote function that
its client offers. The frontend SDK's has two. Each function checks the
input before it sends it, so a wrong value fails at once, in the
browser, and checks the output, so a service that answers with
something else can't go unnoticed:

```console
$ cat out/browser.ts
// Generated from the API description by program.xq and print.xsl. Do not edit.
import { call, expect } from "../runtime.ts";
import type { EvaluationContext, Evaluations, StatusRequest, FlagStatus } from "./types.ts";
import { checkEvaluationContext, checkEvaluations, checkStatusRequest, checkFlagStatus } from "./types.ts";

/** Whether each flag is on for one user, for an SDK in a browser page. */
export async function evaluateFlags(input: EvaluationContext): Promise<Evaluations> {
  expect<EvaluationContext>(input, checkEvaluationContext);
  const output = await call("evaluateFlags", "query", input);
  return expect<Evaluations>(output, checkEvaluations);
}

/** The state of one flag and its failed checks since the last rollout step. */
export async function getStatus(input: StatusRequest): Promise<FlagStatus> {
  expect<StatusRequest>(input, checkStatusRequest);
  const output = await call("getStatus", "query", input);
  return expect<FlagStatus>(output, checkFlagStatus);
}
```

The frontend SDK imports `evaluateFlags` and calls it like any other
function; that it runs in the service is a detail of how it is made.
`node.ts` and `console.ts` look the same, with the functions of the
backend SDK and of the console, and `getStatus` is in both SDKs'
modules. The service's side is an interface, which the service's own
code implements, and a handler that routes each request to it, with
the checks around it. It has every function, whichever client calls
it: a request is checked by the service whether or not it came through
a generated client.

```console
$ cat out/server.ts
// Generated from the API description by program.xq and print.xsl. Do not edit.
import { serve } from "../runtime.ts";
import type { FlagsRequest, FlagSet, EvaluationContext, Evaluations, StatusRequest, FlagStatus, RolloutRequest, KillRequest, KillResult } from "./types.ts";
import { checkFlagsRequest, checkFlagSet, checkEvaluationContext, checkEvaluations, checkStatusRequest, checkFlagStatus, checkRolloutRequest, checkKillRequest, checkKillResult } from "./types.ts";

/** What the service does: one method for each remote function. */
export interface Implementation {
  /** Every flag with its rules, for an SDK that evaluates flags itself. */
  getFlags(input: FlagsRequest): Promise<FlagSet>;
  /** Whether each flag is on for one user, for an SDK in a browser page. */
  evaluateFlags(input: EvaluationContext): Promise<Evaluations>;
  /** The state of one flag and its failed checks since the last rollout step. */
  getStatus(input: StatusRequest): Promise<FlagStatus>;
  /** Rolls a flag out to a new percentage once its guardrail checks pass. */
  rollOutFlag(input: RolloutRequest): Promise<FlagStatus>;
  /** Turns a flag off for every user at once. */
  killFlag(input: KillRequest): Promise<KillResult>;
}

/** A Node.js request listener that checks every input and output. */
export function createHandler(implementation: Implementation) {
  return serve({
    getFlags: {
      kind: "query",
      checkInput: checkFlagsRequest,
      checkOutput: checkFlagSet,
      run: (input) => implementation.getFlags(input),
    },
    evaluateFlags: {
      kind: "query",
      checkInput: checkEvaluationContext,
      checkOutput: checkEvaluations,
      run: (input) => implementation.evaluateFlags(input),
    },
    getStatus: {
      kind: "query",
      checkInput: checkStatusRequest,
      checkOutput: checkFlagStatus,
      run: (input) => implementation.getStatus(input),
    },
    rollOutFlag: {
      kind: "command",
      checkInput: checkRolloutRequest,
      checkOutput: checkFlagStatus,
      run: (input) => implementation.rollOutFlag(input),
    },
    killFlag: {
      kind: "command",
      checkInput: checkKillRequest,
      checkOutput: checkKillResult,
      run: (input) => implementation.killFlag(input),
    },
  });
}
```

The objects inside objects of `createHandler` are indented by the same
rule as the statements: an `entry` is a line, inside one block more for
each `object` around it.

`types.ts` has every type of the schema, as in the pieces of the last
section. The check for a rollout request calls the checks of its three
properties:

```console
$ sed -n '/^export function checkRolloutRequest/,/^}/p' out/types.ts
export function checkRolloutRequest(value: unknown, path: string, problems: string[]): value is RolloutRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkPercentage(value.percentage, path + ".percentage", problems);
  checkOwner(value.owner, path + ".owner", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "percentage", "owner"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
```

The printer reads nothing but the tree: no date, no file name, no
order that could change from one run to the next. So the same tree
always gives the same text, and the same sources the same tree.

## The same tree in another style

A team that wants four spaces, and braces around every block, doesn't
need another generator, or even another printer. A stylesheet imports
`print.xsl` and replaces the two things that decide the layout: the
parameter `$indent` and the function `print:braces`. A declaration in
the importing stylesheet wins over the one it imports, as a template
rule does:

`braces.xsl`:

```xml
{{#include ../examples/codegen/braces.xsl}}
```

The check of a flag set, printed both ways:

```console
$ sxml xquery --method=xml --expression='<module>{ doc("generated/program.xml")//function[@name = "checkFlagSet"] }</module>' > flag-set.xml
$ sxml transform print.xsl flag-set.xml
export function checkFlagSet(value: unknown, path: string, problems: string[]): value is FlagSet {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (Array.isArray(value.flags)) {
    value.flags.forEach((item, index) => checkFlagConfig(item, path + ".flags[" + index + "]", problems));
  } else {
    problems.push(path + ".flags: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["flags"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
$ sxml transform braces.xsl flag-set.xml
export function checkFlagSet(value: unknown, path: string, problems: string[]): value is FlagSet {
    if (!checkObject(value, path, problems)) {
        return false;
    }
    const count = problems.length;
    if (Array.isArray(value.flags)) {
        value.flags.forEach((item, index) => checkFlagConfig(item, path + ".flags[" + index + "]", problems));
    } else {
        problems.push(path + ".flags: expected an array");
    }
    for (const key of Object.keys(value)) {
        if (!["flags"].includes(key)) {
            problems.push(path + "." + key + ": is not allowed");
        }
    }
    return problems.length === count;
}
```

Only the layout changed: the tree is the same, so what the code does is
the same. A style is a few declarations, not a second copy of the
generator.
