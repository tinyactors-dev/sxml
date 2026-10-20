(: Writes a log of $lines lines in the format of log.ixml. :)
declare variable $lines external;
string-join(
  for $number in 1 to xs:integer($lines)
  let $time := xs:time("00:00:00") + xs:dayTimeDuration("PT1S") * ($number mod 86400)
  let $level := ("INFO", "WARN", "ERROR")[$number mod 3 + 1]
  return `2026-10-20 {$time} {$level} message {$number}`,
  "&#10;"
)
