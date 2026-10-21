(: A Latch machine (the context item) as a diagram description for
   draw.xqm: a node for each state, the initial state first, and an edge
   for each pair of states that transitions connect, labelled with their
   events. A transition without a target is an edge from its state to
   itself. With before=FILE, the parts that FILE's machine does not have
   get the class "added", and the parts only FILE has the class
   "removed". :)
declare default element namespace "http://www.w3.org/2005/07/scxml";
declare namespace latch = "urn:example:latch";
declare option output:indent "yes";

declare variable $before as xs:string external := "";

(: The states of a machine, initial first, and its edges as records. :)
declare function local:states($machine as element(scxml)) as xs:string* {
  let $initial := string($machine/@initial)
  return ($initial, $machine//state/@id[. ne $initial] ! string())
};

declare function local:edges($machine as element(scxml)) as record(from, to, label)* {
  for $transition in $machine//transition
  let $from := string($transition/../@id)
  group by $from, $to := string(($transition/@target, $from)[1])
  return { "from": $from, "to": $to,
           "label": string-join(distinct-values($transition/@event ! tokenize(.)), ", ") }
};

declare function local:class($present as xs:boolean, $added as xs:boolean) as attribute()? {
  if ($added and $present) then attribute class { "added" }
  else if (not($present)) then attribute class { "removed" }
  else ()
};

let $machine := /scxml
let $old := if ($before) then doc($before)/scxml else $machine
let $states := local:states($machine)
let $old-states := local:states($old)
let $edges := local:edges($machine)
let $old-edges := local:edges($old)
let $key := fn($edge) { $edge?from || " " || $edge?to }
return <diagram xmlns="" title="{ $machine/@name }">{
  for $state in distinct-values(($states, $old-states))
  return <node id="{ $state }" label="{ $state }">{
    local:class($state = $states, not($state = $old-states))
  }</node>,
  for $edge in ($edges, $old-edges[not($key(.) = $edges ! $key(.))])
  return <edge from="{ $edge?from }" to="{ $edge?to }" label="{ $edge?label }">{
    local:class($key($edge) = $edges ! $key(.), not($key($edge) = $old-edges ! $key(.)))
  }</edge>
}</diagram>
