xquery version "4.0";
(: The layout of a diagram (the context item) as XML: the description
   with the size of the drawing, the place of every box, and the line
   (or, from a node to itself, the loop) of every edge. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";
declare option output:indent "yes";

let $diagram := /diagram
let $layout := draw:layout($diagram)
let $places := $layout?places
return <diagram>{
  $diagram/@*,
  attribute width { $layout?width },
  attribute height { $layout?height },
  for $node in $diagram/node
  let $place := $places($node/@id)
  return <node>{ $node/@*, attribute x { $place?x }, attribute y { $place?y } }</node>,
  for $edge in $diagram/edge
  let $from := $places($edge/@from)
  let $line := draw:line($from, $places($edge/@to))
  return <edge>{
    $edge/@*,
    if (exists($line)) then
      for $name at $position in ("x1", "y1", "x2", "y2")
      return attribute { $name } { $line[$position] }
    else attribute d { draw:loop($from) }
  }</edge>
}</diagram>
