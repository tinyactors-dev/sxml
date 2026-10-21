declare option output:method "text";
declare option output:item-separator "&#10;";

(:~ The drafts to look at: file names, separated by spaces. :)
declare variable $drafts as xs:string external := "latch.xml";

(:~ The day to compute from: give it with --param=today=YYYY-MM-DD. :)
declare variable $today as xs:date external := current-date();

(:~ How long an Internet-Draft is valid (RFC 2026, 2.2). :)
declare variable $days as xs:integer external := 185;

declare variable $months := ("January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December");

for $file in tokenize($drafts)
let $draft := doc($file)/rfc
let $date := $draft/front/date
let $published := xs:date(`{$date/@year}-{format-integer(index-of($months, $date/@month), "00")}-{format-integer($date/@day, "00")}`)
let $expires := $published + xs:dayTimeDuration("P" || $days || "D")
return `{$draft/@docName}: published {$published}, expires {$expires}{
  if ($expires lt $today) then ", expired" else ""
}`
