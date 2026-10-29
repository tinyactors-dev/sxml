xquery version "4.0";

(:~
 : tools/spec/spec status [DOCUMENT]: the state of the packets and gates.
 : Without DOCUMENT, one line per task document (docs/tasks/); with it,
 : each of its packets and the open boxes of its gate.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $name as xs:string external := "";

declare function local:counts($packets as element()*) as xs:string {
  string-join(
    for $packet in $packets
    group by $state := tokenize(spec:status($packet))[1] || (if (spec:status($packet) = "not recorded") then " recorded" else "")
    order by $state
    return count($packet) || " " || $state,
    ", ")
};

let $spec := spec:load()
let $tasks := spec:of-kind($spec, "document")[starts-with(@id, "tasks/")]
return string-join(
  if ($name = "") then (
    for $document in $tasks
    let $packets := $document//include[@kind = "packet"] ! spec:target($spec, .)
    let $boxes := spec:boxes($document)
    return $document/@id || ": " || count($packets) || " packets (" || local:counts($packets) || ")" ||
      (if (sum($boxes) > 0) then "; boxes " || $boxes[1] || " of " || sum($boxes) || " checked" else ""))
  else (
    let $document := spec:one($spec, if (contains($name, "/")) then $name else "tasks/" || $name)
    return (
      spec:line($document),
      for $include in $document//include[@kind = "packet"]
      let $packet := spec:target($spec, $include)
      return "  " || string-join(($packet/@id, "[" || spec:status($packet) || "]", spec:title($packet)), " "),
      for $item in $document//item[@checked = "false"]
      return "  open box (" || ($item/ancestor::section)[last()]/@id || "): " || normalize-space(spec:plain($item/p[1]))
    )),
  "&#10;")
