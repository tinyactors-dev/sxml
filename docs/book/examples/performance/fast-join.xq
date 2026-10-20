(: Build an index once, then look up in it. :)
let $by-id := map:build(//order, fn { xs:integer(@id) })
return count(
  for $order in //order
  let $next := $by-id(xs:integer($order/@id) + 1)
  where $next/customer = "c3"
  return $order
)
