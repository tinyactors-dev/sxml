(: The term "SDK" becomes "Client": its definition, its anchor, and
   every cross reference to it with its text. :)
let $term := //dt[@anchor = "term-sdk"]
return (
  replace value of node $term with "Client",
  replace value of node $term/@anchor with "term-client",
  for $xref in //xref[@target = "term-sdk"]
  return (
    replace value of node $xref/@target with "term-client",
    replace value of node $xref with "client"
  )
)
