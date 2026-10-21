(: The flag's numbers, and the requirement that states each of them in
   words: ten minutes, three failed checks, five minutes. :)
let $numbers := { "req-3": 10, "req-7": 3, "req-8": 5 }
for $anchor in map:keys($numbers)
let $words := format-integer($numbers($anchor), "w")
let $requirement := //t[@anchor = $anchor]
return `{$anchor} states {$words}: {$requirement contains text { $words }}`
