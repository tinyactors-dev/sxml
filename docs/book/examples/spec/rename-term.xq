(: The term "Controller" becomes "Hub": its definition, its anchor, and
   every cross reference to it with its text. :)
let $term := //dt[@anchor = "term-controller"]
return (
  replace value of node $term with "Hub",
  replace value of node $term/@anchor with "term-hub",
  for $xref in //xref[@target = "term-controller"]
  return (
    replace value of node $xref/@target with "term-hub",
    replace value of node $xref with "hub"
  )
)
