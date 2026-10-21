xquery version "4.0";
(: The rows of diagram.xml (the context item): one array of node ids per rank. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";

let $ranks := draw:ranks(/diagram)
for $node in /diagram/node
group by $rank := $ranks($node/@id)
order by $rank
return array { $node/@id/string() }
