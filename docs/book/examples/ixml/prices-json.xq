(: Turns prices.txt into JSON: one object per row, keyed by the header. :)
declare option output:method "json";
declare option output:indent "yes";
let $table := invisible-xml(unparsed-text("prices.ixml"))(unparsed-text("prices.txt"))
let $names := $table//header/field ! string()
return array {
  for $row in $table//row
  return map:merge(
    for $field at $position in $row/field
    return map:entry($names[$position],
      if ($field castable as xs:decimal) then xs:decimal($field) else string($field)))
}
