declare option output:indent "yes";

<requirements>{
  for $t in //t[@anchor]
  order by $t/bcp14, $t/@anchor
  return <requirement anchor="{ $t/@anchor }" keyword="{ $t/bcp14 }">{
    normalize-space($t)
  }</requirement>
}</requirements>
