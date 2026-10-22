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
  let $box := $layout?boxes($node/@id)
  let $shape := if ($node/@kind = "store") then "ellipse" else "rect"
  return element { $shape } {
    if ($shape = "ellipse") then (
      attribute cx { $box?x + $box?width idiv 2 },
      attribute cy { $box?y + $box?height idiv 2 },
      attribute rx { $box?width idiv 2 }, attribute ry { $box?height idiv 2 }
    ) else (
      attribute x { $box?x }, attribute y { $box?y },
      attribute width { $box?width }, attribute height { $box?height }
    ),
    attribute fill { "none" },
    attribute stroke { "black" }
  }
}</svg>
