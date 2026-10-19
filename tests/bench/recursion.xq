(: Deep recursion: a non-tail recursive function 100,000 calls deep, a tail
   recursive one 300,000 deep, and a recursive fold. :)
declare function local:depth($n as xs:integer) as xs:integer {
  if ($n = 0) then 0 else 1 + local:depth($n - 1)
};
declare function local:loop($n as xs:integer, $sum as xs:integer) as xs:integer {
  if ($n = 0) then $sum else local:loop($n - 1, $sum + $n)
};
(
  local:depth(100000),
  local:loop(300000, 0),
  fold-left(1 to 300000, 0, function($a, $b) { $a + $b mod 7 })
)
