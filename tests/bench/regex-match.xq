(: Regex matching (perf-7): fn:matches on short strings, where matching,
   not compiling (D308), is the cost. Category escapes, explicit ranges,
   a literal prefix, a search whose match starts late, and a regex that
   fails at every start position. :)
(
  count((1 to 200000)[matches("abc_def ghi " || ., "\w+\s\w+\s\d+")]),
  count((1 to 200000)[matches("abc_def ghi " || ., "[a-z_]+ [a-z]+ [0-9]+")]),
  count((1 to 100000)[matches("item-" || . || "-end", "^item-\d+-end$")]),
  count((1 to 100000)[matches("the quick brown fox " || ., "fox \d*7$")]),
  count((1 to 100000)[matches("no digits here at all, none " || ., "[xyz]\d")])
)
