xquery version "4.0";
(: The boxes of diagram.xml (the context item) at their places, with
   stores drawn as ellipses: the element's name is computed. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";
declare default element namespace "http://www.w3.org/2000/svg";
declare option output:indent "yes";

let $diagram := /Q{}diagram
let $layout := draw:layout($diagram)
return <svg width="{ $layout?width }" height="{ $layout?height }">{
  for $node in $diagram/Q{}node
  let $place := $layout?places($node/@id)
  let $shape := if ($node/@kind = "store") then "ellipse" else "rect"
  return element { $shape } {
    if ($shape = "ellipse") then (
      attribute cx { $place?x + 70 }, attribute cy { $place?y + 20 },
      attribute rx { 70 }, attribute ry { 20 }
    ) else (
      attribute x { $place?x }, attribute y { $place?y },
      attribute width { 140 }, attribute height { 40 }
    ),
    attribute fill { "none" },
    attribute stroke { "black" }
  }
}</svg>
