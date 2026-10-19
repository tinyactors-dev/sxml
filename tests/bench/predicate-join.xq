(: A join written as a filter: for each person, the closed auctions whose
   buyer is that person, $seq[f(.) = $key] (the shape of app-Demos itunes,
   D182). :)
let $closed := //closed_auction
for $p in //person
return count($closed[buyer/@person = $p/@id])
