xquery version "4.0";
(: Draws diagram.xml (the context item) as SVG. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";
declare option output:indent "yes";

draw:svg(/diagram)
