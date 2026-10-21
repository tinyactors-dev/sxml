(: Requirement anchors in document order: the requirement a reader sees
   as number 6 gets the anchor req-6, and every cross reference to a
   requirement follows its anchor. :)
for $requirement at $number in //t[@anchor][bcp14]
let $old := string($requirement/@anchor)
let $new := "req-" || $number
where $new ne $old
return (
  replace value of node $requirement/@anchor with $new,
  for $xref in //xref[@target = $old]
  return replace value of node $xref/@target with $new
)
