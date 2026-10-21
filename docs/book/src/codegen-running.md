# Running the Generated Code

<!-- examples: codegen -->
<!-- requires: javascript -->

Generated code is only as good as what it does when it runs. In this
section we'll start a small Vane service with the generated handler,
and call it with the generated clients, the way the two SDKs and the
console do. The examples need Node.js 22.18 or later, which runs
TypeScript files directly by removing their types; the generated code
uses no TypeScript feature that needs more than that. The files that
`generate.sh` makes are in the `generated` directory of the examples
(the next section shows the script).

## The service's own code

The generated interface says what the service must do; the service's
code does it. The real Vane service keeps its flags in PostgreSQL, in
its registry module; this one keeps three in an array:

`vane.ts`:

```ts
{{#include ../examples/codegen/vane.ts}}
```

The parameters need no type annotations: the interface gives them.
Mistype a property name, or return a status without its failed checks,
and the TypeScript compiler finds it, because the types come from the
schema. The service's flags have more than the API shows, an owner list
and a count of failed checks, so `getFlags` copies only the four
properties of a `FlagConfig`. Forget that, and the generated handler's
output check refuses the answer with "is not allowed": the service
can't hand its owners' addresses to every SDK by mistake.

`evaluateFlags` is what the frontend SDK asks for. A rule whose
attribute and value match the user gives the percentage; otherwise the
flag's own percentage does, and `place` puts the user in the share or
out of it, by a hash of the flag and the user, so the same user gets the
same answer on every call. The backend SDK does the same evaluation
itself, on the flags that `getFlags` gave it.

## The SDKs on top

The generated clients are the transport of the SDKs: they send a call,
check what goes and what comes back, and nothing more. What makes an
SDK, such as a cache, is written by hand on top of them. Here is a
piece of the backend SDK. The Vane protocol says that an SDK sends at
most one status request per second for a flag and uses its cached
answer in between:

`sdk.ts`:

```ts
{{#include ../examples/codegen/sdk.ts}}
```

## Calling it

`demo.ts` starts the service on a free port, points the clients at it,
and calls the functions as their callers would. To show what the SDK's
cache saves, it counts the status requests that the service answers:
`Implementation` is an ordinary interface, so an object that wraps one
method of `vane` is an implementation too. Then it tries two calls that
should fail:

`demo.ts`:

```ts
{{#include ../examples/codegen/demo.ts}}
```

```console
$ node demo.ts
getFlags {"flags":[{"key":"new-checkout","state":"rolling-out","percentage":20,"rules":[{"attribute":"country","values":["nl"],"percentage":100}]},{"key":"dark-mode","state":"stable","percentage":100,"rules":[]},{"key":"beta-banner","state":"paused","percentage":5,"rules":[]}]}
evaluateFlags {"evaluations":[{"flag":"new-checkout","on":true},{"flag":"dark-mode","on":true},{"flag":"beta-banner","on":false}]}
flagStatus {"flag":"beta-banner","state":"paused","failedChecks":3,"retryAfter":300}
flagStatus {"flag":"beta-banner","state":"paused","failedChecks":3,"retryAfter":300}
status requests: 1
killFlag {"flag":"new-checkout","killed":true}
evaluateFlags {"evaluations":[{"flag":"new-checkout","on":false},{"flag":"dark-mode","on":true},{"flag":"beta-banner","on":false}]}
rollOutFlag failed: input.percentage: must be at most 100; input.owner: must match [^@ ]+@[^@ ]+
fetch 400 {"problems":["input.flag: must match [a-z0-9][a-z0-9-]*","input.force: is not allowed"]}
```

The backend SDK gets every flag with its rules, but no owners. The user
`user-7` from the Netherlands gets `new-checkout` from its country rule
and `dark-mode` from its 100 percent; `beta-banner` is rolled out to 5
percent, and this user's place isn't in them. The two status calls give
the same answer, `beta-banner` paused after three failed checks, to be
asked about again in 300 seconds, and the service answered only one of
them. After the
console's kill, `new-checkout` is off for this user too: a kill sets the
percentage to 0 and drops the rules.

The next call has the right TypeScript types, strings and a number,
but values that the schema's facets refuse, a percentage of 150 and an
owner without a domain, and the client stops it before it goes over the
network. The last request doesn't use a client at all, as a request
from another program wouldn't, and the service refuses it with the same
rules, and an answer that says why: a flag key is written in lowercase,
and a kill has no `force`.

The frontend SDK uses `browser.ts` the same way in a browser page, with
`settings.endpoint` left at `/api`, and the service runs the same
handler, whatever framework serves the requests.
