(: Big FLWOR expressions: group by over 200,000 tuples, and group by over
   the items of the auction document. :)
(
  <groups>{
    for $i in 1 to 200000
    let $k := $i mod 997
    group by $k
    order by $k
    return <g k="{$k}" n="{count($i)}" s="{sum($i)}"/>
  }</groups>,
  <locations>{
    for $item in //item
    group by $location := string($item/location)
    order by count($item) descending, $location
    return <location name="{$location}" count="{count($item)}"/>
  }</locations>
)
