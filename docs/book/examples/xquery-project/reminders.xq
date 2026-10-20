(:~ One reminder file for each member with overdue loans. :)
import module namespace lib = "https://example.org/library" at "library.xqm";

declare variable $today as xs:date external := current-date();

let $library := doc("library.xml")
let $members := json-doc("members.json")
for $info in lib:loan-infos(doc("loans.xml")//loan, $library, $members)
where lib:is-overdue($info, $today)
group by $member := $info?member
return put(
  <reminder to="{ $info[1]?name }" date="{ $today }">{
    $info ! <book due="{ ?due }">{ ?title }</book>
  }</reminder>,
  "reminder-" || $member || ".xml"
)
