(: Draws a diagram (the context item) with the drawing module. :)
import module namespace draw = "urn:example:draw" at "draw.xqm";
declare option output:indent "yes";

draw:svg(/diagram)
