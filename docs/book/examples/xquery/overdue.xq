declare option output:method "text";
declare option output:item-separator "&#10;";

(:~ The day to compute from: give it with --param=today=YYYY-MM-DD. :)
declare variable $today as xs:date external := current-date();

(:~ How long a loan lasts. :)
declare variable $days as xs:integer external := 21;

for $loan in doc("loans.xml")//loan
let $due := xs:date($loan/@date) + xs:dayTimeDuration("P" || $days || "D")
where $due lt $today
order by $due
return `{ $loan/@member } has { $loan/@book } since { $loan/@date }, due { $due }`
