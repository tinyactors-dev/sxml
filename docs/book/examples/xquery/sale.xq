(: Ten percent off every book, and no more prices with 9 cents. :)
for $price in //price
let $new := round(xs:decimal($price) * 0.9, 2)
return replace value of node $price with $new
