(: Big FLWOR expressions: order by an integer key over 200,000 tuples, by a
   string key over the persons, and by two keys over the bidders. :)
(
  sum(subsequence(
    for $i in 1 to 200000 order by ($i * 7919) mod 100003 return $i, 1, 1000)),
  string-join(
    for $p in //person order by string($p/name) return string($p/@id), " "),
  count(
    for $b in //bidder
    order by xs:decimal($b/increase) descending, string($b/date)
    return $b)
)
