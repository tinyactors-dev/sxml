(: Status comes before Unlock now. The section moves with everything in
   it, and with the line break and indentation in front of it. :)
let $status := //section[@anchor = "status"]
let $unlock := //section[@anchor = "unlock"]
let $indentation := $status/preceding-sibling::node()[1][self::text()]
return (
  insert nodes ($status, $indentation) before $unlock,
  delete nodes ($status, $indentation)
)
