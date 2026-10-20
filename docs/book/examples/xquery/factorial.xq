declare function local:factorial($n as xs:integer) as xs:integer {
  if ($n le 1) then 1 else $n * local:factorial($n - 1)
};

(1 to 5) ! local:factorial(.), local:factorial(30)
