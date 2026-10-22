xquery version "4.0";
(: The layout of a diagram (the context item) as XML: the description
   with the size of the drawing, the box of every node, and the path of
   every edge, with the place of its label. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";
declare option output:indent "yes";

let $diagram := /diagram
let $layout := draw:layout($diagram)
return <diagram>{
  $diagram/@*,
  attribute width { $layout?width },
  attribute height { $layout?height },
  for $node in $diagram/node
  let $box := $layout?boxes($node/@id)
  return <node>{
    $node/@*,
    for $name in ("x", "y", "width", "height")
    return attribute { $name } { $box($name) }
  }</node>,
  for $edge at $position in $diagram/edge
  let $route := $layout?routes($position)
  return <edge>{
    $edge/@*,
    attribute d { $route?path },
    $route?label ! (attribute label-x { ?x }, attribute label-y { ?y })
  }</edge>
}</diagram>
