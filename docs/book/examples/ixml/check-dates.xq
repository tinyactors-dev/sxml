(: Reads each line of the file $file as a date and reports the lines that
   are not dates, instead of stopping at the first. :)
declare variable $file external;
let $parse := invisible-xml(unparsed-text("date.ixml"), {"fail-on-error": true()})
return string-join(
  for $line at $number in unparsed-text-lines($file)
  return try {
    $parse($line) ! ()
  } catch err:FOIX0002 {
    `line {$number}: {$line} is not a date`
  },
  "&#10;")
