<receipt>{
  for $line in //line
  let $amount := xs:decimal($line/quantity)
               * xs:decimal($line/price)
  order by $amount descending
  return
    <item quantity="{$line/quantity}"
          amount="{$amount}">{string($line/item)}</item>
}</receipt>
