(: Required literals in regex search (perf-9): regexes that every match
   must spell a literal through, on texts where it seldom occurs. As in
   SchXslt's expansion of abstract Schematron patterns, "(\W*)\$name(\W*)"
   can match the empty string before a "$", so the start filter skips
   nothing; the required literal "$name" is looked for first, and a text
   without it is given up at once. Then a literal after a class
   repetition, a literal separator in tokenize, and a literal in the
   middle of an alternation-free regex with groups. :)
let $names := (1 to 400) ! ("PARAM" || .)
let $texts := (1 to 25) ! ("(cbc:TaxAmount/@currencyID = $PARAM" || (. * 31 mod 400 + 1)
                            || ") and count(cac:TaxSubtotal) &gt; 0 or not(cbc:ID = $X" || . || ")")
return (
  sum(for $text in $texts
      return string-length(fold-left($names, $text, function($s, $name) {
        replace($s, "(\W*)\$" || $name || "(\W*)", "$1[" || $name || "]$2")
      }))),
  count((1 to 100000)[matches("customer number " || . || " at example dot org", "\w+@\w+\.org")]),
  sum(for $i in 1 to 20000
      return count(tokenize("alpha beta gamma delta epsilon zeta eta theta " || $i, "\s*;\s*"))),
  count((1 to 50000)[matches("ref " || . || ": nothing to see here, move along", "(\d+)-(\d+)-x")])
)
