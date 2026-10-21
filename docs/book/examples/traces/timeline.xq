(: The starts and ends of the spans of a file in the normal form, in
   the order they happened, up to the moment $until. :)
declare option output:indent "yes";
declare variable $until external := "9999-12-31T00:00:00Z";

<timeline>{
  for $span in //span
  let $start := xs:dateTime($span/ancestor::trace/@start)
                + xs:dayTimeDuration("PT0.001S") * xs:decimal($span/@start)
  let $end := $start + xs:dayTimeDuration("PT0.001S") * xs:decimal($span/@duration)
  for $event in (
    <start at="{ $start }" span="{ $span/@id }" name="{ $span/@name }"
           root="{ empty($span/parent::span) }"/>,
    <end at="{ $end }" span="{ $span/@id }"/>
  )
  where xs:dateTime($event/@at) le xs:dateTime($until)
  stable order by xs:dateTime($event/@at)
  return $event
}</timeline>
