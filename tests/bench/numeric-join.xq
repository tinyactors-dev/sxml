(: A join on numbers (packet perf-2, D229): every person with an income
   against the persons of the same income, compared as xs:decimal. :)
let $earners := /site/people/person[profile/@income]
return sum(
  for $p in $earners
  return count($earners[xs:decimal(profile/@income) = xs:decimal($p/profile/@income)]))
