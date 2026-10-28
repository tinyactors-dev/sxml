(: Recursion over the rest of a sequence (packet perf-5): 100 walks over
   780 elements, as SchXslt's schxslt:replace-params walks its parameters,
   in each of three forms: $items[position() > 1], fn:tail and
   fn:subsequence. A slice of a stored sequence shares its items, and a
   typed parameter matches the slice once its base matched, so each walk
   is linear in the length (quadratic before). :)
declare function local:walk-filter($items as element()*, $total as xs:integer) as xs:integer {
  if (empty($items)) then $total else local:walk-filter($items[position() > 1], $total + 1)
};
declare function local:walk-tail($items as element()*, $total as xs:integer) as xs:integer {
  if (empty($items)) then $total else local:walk-tail(tail($items), $total + 1)
};
declare function local:walk-subsequence($items as element()*, $total as xs:integer) as xs:integer {
  if (empty($items)) then $total
  else local:walk-subsequence(subsequence($items, 2), $total + 1)
};
let $document := document { <pattern>{ (1 to 780) ! <param name="p{.}"/> }</pattern> }
let $params := $document/pattern/param
return (
  sum(for $walk in 1 to 100 return local:walk-filter($params, 0)),
  sum(for $walk in 1 to 100 return local:walk-tail($params, 0)),
  sum(for $walk in 1 to 100 return local:walk-subsequence($params, 0))
)
