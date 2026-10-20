(: The join of the book's performance chapter (packet perf-4, D259): for
   each of 2,000 orders, the order with the next id, found by
   //order[@id = ...], a descendant path whose untyped keys meet a
   numeric probe. :)
let $orders := document {
  <orders>{
    for $id in 1 to 2000
    return <order id="{ $id }"><customer>c{ $id mod 7 }</customer></order>
  }</orders>
}
return count(
  for $order in $orders//order
  let $next := $orders//order[@id = $order/@id + 1]
  where $next/customer = "c3"
  return $order
)
