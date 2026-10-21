(: The diff of two versions (diff.xml, the context item) as a diagram
   for draw.xqm: the functions of the program and the calls between
   them, each with the class of its change. :)
declare option output:indent "yes";

(: The functions in the order the chains first go through them, so that
   the entry is the first node and the drawing reads like the chains. :)
let $order := distinct-values(/diff/chain ! tokenize(translate(., "*", ""), " > "))
return <diagram title="Call graph, { /diff/@old } and { /diff/@new }">{
  for $function in /diff/function
  order by index-of($order, $function/@id)
  return <node id="{ $function/@id }" label="{ $function/@id }">{
    $function/@change ! attribute class { . }
  }</node>,
  for $call in /diff/call[not(@external)]
  return <edge from="{ $call/@from }" to="{ $call/@to }">{
    if ($call/@repeated) { attribute label { "loop" } },
    $call/@change ! attribute class { . }
  }</edge>
}</diagram>
