(: What changed between two trace files, as text. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $before external := "before.json";
declare variable $after external := "after.json";

let $old := trace:load($before)
let $new := trace:load($after)
return (
  for $version in trace:versions($old, $new)
  return `{ $version?service }: { $version?before otherwise "none" } -> { $version?after otherwise "none" }`,
  for $change in trace:diff($old, $new)
  return `{ $change?route }: { $change?caller } -> { $change?callee }  { $change?before } -> { $change?after }`
)
