(: The SHOULD in Security Considerations becomes a requirement with an
   anchor of its own, and the editorial note that asked for it goes. :)
let $paragraph := //section[@anchor = "security"]/t[bcp14]
return (
  insert node attribute anchor { "req-12" } into $paragraph,
  delete node $paragraph/cref[@anchor = "issue-2"]
)
