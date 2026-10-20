(: Counts the lines of each level in app.log and lists the errors. :)
let $parse := invisible-xml(unparsed-text("log.ixml"))
let $log := $parse(unparsed-text("app.log"))
return string-join((
  for $line in $log//line
  group by $level := $line/@level
  order by count($line) descending, $level
  return `{$level}: {count($line)}`,
  $log//line[@level = "ERROR"] ! `{@time} {message}`
), "&#10;")
