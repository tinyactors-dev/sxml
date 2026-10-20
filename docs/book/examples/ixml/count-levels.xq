(: Parses the log in the file $file and counts its lines by level. :)
declare variable $file external;
let $log := invisible-xml(unparsed-text("log.ixml"))(unparsed-text($file))
return string-join(
  for $line in $log//line
  group by $level := string($line/@level)
  order by $level
  return `{$level} {count($line)}`,
  "&#10;")
