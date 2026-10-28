(: Regex reuse (D308): fn:replace, fn:matches, fn:tokenize and
   fn:analyze-string called many times with few regexes, and fn:replace
   cycling through 800 regexes built from parameter names, as SchXslt's
   expansion of abstract Schematron patterns does. Each regex is compiled
   once per evaluation. :)
let $names := (1 to 800) ! ("PARAM" || .)
let $texts := (1 to 25) ! ("cbc:Amount = $PARAM" || (. * 31 mod 800 + 1) || " and $X" || .)
return (
  sum(for $i in 1 to 20000
      return string-length(replace("cbc:Amount = $AMOUNT and $X" || $i,
                                   "(\W*)\$AMOUNT(\W*)", "$1x$2"))),
  count((1 to 20000)[matches(string(.), "^[1-9][0-9]*7$")]),
  sum(for $i in 1 to 5000 return count(tokenize("a, b,c ,  d" || $i, "\s*,\s*"))),
  sum(for $i in 1 to 2000
      return count(analyze-string("key" || $i || "=value" || $i, "(\w+)=(\w+)")//fn:group)),
  sum(for $text in $texts
      return string-length(fold-left($names, $text, function($s, $name) {
        replace($s, "(\W*)\$" || $name || "(\W*)", "$1[" || $name || "]$2")
      })))
)
