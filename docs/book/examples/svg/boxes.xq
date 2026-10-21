xquery version "4.0";
(: One box per node of a diagram (the context item), in a column. :)
declare default element namespace "http://www.w3.org/2000/svg";
declare option output:indent "yes";

let $nodes := //Q{}node
return <svg width="180" height="{ count($nodes) * 60 + 20 }">{
  for $node at $position in $nodes
  let $y := $position * 60 - 40
  return (
    <rect x="20" y="{ $y }" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>,
    <text x="90" y="{ $y + 25 }" text-anchor="middle">{ string($node/@label) }</text>
  )
}</svg>
