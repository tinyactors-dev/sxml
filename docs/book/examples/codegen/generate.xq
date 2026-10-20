(:~ Writes the TypeScript for an API description into a directory. :)
import module namespace ts = "https://example.org/typescript" at "typescript.xqm";

declare variable $api-file as xs:string external := "api.xml";
declare variable $directory as xs:string external := "generated";

let $api := doc($api-file)
let $schema := doc(resolve-uri($api/*/@types, base-uri($api)))
let $files := {
  "types.ts": ts:types($schema),
  "client.ts": ts:client($api),
  "server.ts": ts:server($api)
}
for $name in sort(map:keys($files))
return put(
  document { $files($name) },
  $directory || "/" || $name,
  { "method": "text" }
)
