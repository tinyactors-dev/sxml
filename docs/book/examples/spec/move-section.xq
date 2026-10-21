(: Status comes before Rollout now. The section moves with everything in
   it, and with the line break and indentation in front of it. :)
let $status := //section[@anchor = "status"]
let $rollout := //section[@anchor = "rollout"]
let $indentation := $status/preceding-sibling::node()[1][self::text()]
return (
  insert nodes ($status, $indentation) before $rollout,
  delete nodes ($status, $indentation)
)
