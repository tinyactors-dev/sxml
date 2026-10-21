(: The diff of the call graphs and call chains of two versions: every
   function, call and chain of either version, each marked added,
   removed or changed, and how often the functions of both versions
   call other modules, where that changed. :)
import module namespace graph = "urn:example:graph" at "graph.xqm";

declare option output:indent "yes";

declare variable $old as xs:string external := "before";
declare variable $new as xs:string external := "after";

(: ANCHOR: calls :)
(: The calls of a graph, one per caller and callee, keyed by both; a
   call is repeated when any of its calls is in a loop. :)
declare function local:calls($graph as element(graph)) as map(*) {
  map:merge(
    for $call in $graph/call
    group by $key := $call/@from || " > " || $call/@to
    return { $key: <call>{
      $call[1]/(@from, @to, @external),
      if ($call/@repeated) { attribute repeated { "true" } }
    }</call> }
  )
};
(: ANCHOR_END: calls :)

(: ANCHOR: compare :)
(: Marks each key of either map: added, removed, or changed when the
   two items differ. :)
declare function local:compare($old as map(*), $new as map(*)) as element()* {
  for $key in distinct-values((map:keys($old), map:keys($new)))
  order by $key
  let $item := ($new($key), $old($key))[1]
  let $change :=
    if (not(map:contains($old, $key))) then "added"
    else if (not(map:contains($new, $key))) then "removed"
    else if (not(deep-equal($old($key), $new($key)))) then "changed"
    else ()
  return element { node-name($item) } {
    $item/@*, $change ! attribute change { . }, $item/node()
  }
};
(: ANCHOR_END: compare :)

(: ANCHOR: chains :)
(: The chains of a graph, keyed by the functions they go through. :)
declare function local:chains($graph as element(graph)) as map(*) {
  map:merge(
    for $chain in graph:chains($graph)
    let $text := graph:chain-text($chain)
    return { translate($text, "*", ""): <chain>{ $text }</chain> }
  )
};
(: ANCHOR_END: chains :)

(: ANCHOR: modules :)
(: How many times a function makes some calls: "2" for two calls, "1 +
   n" when one more is in a loop (n times, once per iteration), "0" for
   none. :)
declare function local:count($calls as element(call)*) as xs:string {
  let $once := count($calls[not(@repeated)])
  let $loops := count($calls[@repeated])
  return string-join((
    if ($once > 0 or $loops = 0) { string($once) },
    if ($loops > 0) { (if ($loops > 1) then $loops else "") || "n" }
  ), " + ")
};

(: For each function of both versions and each other module of the
   program it calls, the calls to that module before and after, where
   they differ. :)
declare function local:modules(
  $before as element(graph),
  $after as element(graph)
) as element(module)* {
  for $function in $before/function/@id[. = $after/function/@id]
  let $module-of := fn($id) { substring-before($id, ".") }
  let $calls := fn($graph as element(graph)) as element(call)* {
    $graph/call[@from = $function][not(@external)]
               [$module-of(@to) ne $module-of($function)]
  }
  for $module in sort(distinct-values(($calls($before), $calls($after))/$module-of(@to)))
  let $old := local:count($calls($before)[$module-of(@to) = $module])
  let $new := local:count($calls($after)[$module-of(@to) = $module])
  where $old ne $new
  return <module from="{ $function }" name="{ $module }" old="{ $old }" new="{ $new }"/>
};
(: ANCHOR_END: modules :)

(: ANCHOR: main :)
let $before := graph:graph(graph:read($old))
let $after := graph:graph(graph:read($new))
let $changes := (
  local:compare(
    map:merge($before/function ! { string(@id): <function id="{ @id }"/> }),
    map:merge($after/function ! { string(@id): <function id="{ @id }"/> })),
  local:compare(local:calls($before), local:calls($after)),
  local:compare(local:chains($before), local:chains($after))
)
return <diff old="{ $old }" new="{ $new }">{
  $changes, local:modules($before, $after)
}</diff>
(: ANCHOR_END: main :)
