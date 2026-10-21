xquery version "4.0";
(: The ranks of diagram.xml (the context item) with a fold: one pass per
   node, each pass ranking the nodes one edge away from the last rank. :)
let $diagram := /diagram
let $starts := ($diagram/node[1], $diagram/node[not(@id = $diagram/edge/@to)])/@id
return fold-left(
  1 to count($diagram/node),
  map:build($starts, string#1, fn { 0 }),
  fn($ranks, $pass) {
    let $frontier := map:keys($ranks)[$ranks(.) = $pass - 1]
    let $next := distinct-values($diagram/edge[@from = $frontier]/@to)
      [not(map:contains($ranks, .))]
    return map:merge(($ranks, map:build($next, value := fn { $pass })))
  }
)
