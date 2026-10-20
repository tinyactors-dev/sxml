(:~ The same facts as report.xq, as JSON for other programs. :)
import module namespace lib = "https://example.org/library" at "library.xqm";

declare option output:method "json";
declare option output:indent "yes";

declare variable $today as xs:date external := current-date();

let $library := doc("library.xml")
let $members := json-doc("members.json")
let $infos := lib:loan-infos(doc("loans.xml")//loan, $library, $members)
return {
  "date": string($today),
  "loans": count($infos),
  "overdue": array {
    for $info in $infos[lib:is-overdue(., $today)]
    order by $info?due
    return { "member": $info?member, "title": $info?title, "due": string($info?due) }
  },
  "popular": array {
    for $info in $infos
    group by $title := $info?title
    order by count($info) descending, $title
    count $rank
    where $rank le 3
    return $title
  }
}
