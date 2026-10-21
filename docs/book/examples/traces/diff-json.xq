(: What changed between two trace files, as JSON. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:method "json";
declare option output:indent "yes";
declare option output:escape-solidus "no";

declare variable $before external := "before.json";
declare variable $after external := "after.json";

let $old := trace:load($before)
let $new := trace:load($after)
return {
  "before": $before,
  "after": $after,
  "versions": array { trace:versions($old, $new) },
  "changes": array { trace:diff($old, $new) }
}
