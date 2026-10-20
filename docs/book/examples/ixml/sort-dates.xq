(: Reads dates.txt as xs:date values and lists them in order. :)
let $dates := invisible-xml(unparsed-text("dates.ixml"))(unparsed-text("dates.txt"))
for $date in $dates//date
let $value := xs:date(`{$date/year}-{$date/month}-{$date/day}`)
order by $value
return format-date($value, "[D] [MNn] [Y], a [FNn]")
