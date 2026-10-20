(: For each order, look up the next order by id: a scan each time. :)
count(
  for $order in //order
  let $next := //order[@id = $order/@id + 1]
  where $next/customer = "c3"
  return $order
)
