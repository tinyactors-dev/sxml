(: The participants of one route, before and after, as a diagram for
   draw.xqm: what was added, removed or changed has a class, and a
   database is drawn as a store. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:indent "yes";

declare variable $before external := "before.json";
declare variable $after external := "after.json";
declare variable $route external := "GET /api/getFlags";

(: The calls of the route, from a map of cardinalities. :)
declare function local:calls($cardinality as map(*)) as map(*) {
  map:filter($cardinality, fn($key, $edge) { $edge?route = $route })
};

(: Whether a service takes part in one of the calls. :)
declare function local:takes-part($name as xs:string, $calls as map(*)) as xs:boolean {
  some $edge in map:items($calls) satisfies $name = ($edge?caller, $edge?callee)
};

let $old := trace:load($before)
let $new := trace:load($after)
let $old-calls := local:calls(trace:cardinality($old))
let $new-calls := local:calls(trace:cardinality($new))
let $calls := map:merge(($old-calls, $new-calls))
let $databases := ($old, $new)//span[@db]/@peer
return <diagram title="{ $route }: { $before } and { $after }">{
  for $name in distinct-values(map:items($calls) ! (?caller, ?callee))
  return <node id="{ $name }" label="{ $name }">{
    if ($name = $databases) then attribute kind { "store" } else (),
    if (not(local:takes-part($name, $old-calls))) then attribute class { "added" }
    else if (not(local:takes-part($name, $new-calls))) then attribute class { "removed" }
    else ()
  }</node>,
  for $key in map:keys($calls)
  let $count-before := $old-calls($key)?count otherwise 0
  let $count-after := $new-calls($key)?count otherwise 0
  return <edge from="{ $calls($key)?caller }" to="{ $calls($key)?callee }">{
    if ($count-before = $count-after) then attribute label { $count-after }
    else (
      attribute label { `{ $count-before } → { $count-after }` },
      attribute class {
        if ($count-before = 0) then "added"
        else if ($count-after = 0) then "removed"
        else "changed"
      }
    )
  }</edge>
}</diagram>
