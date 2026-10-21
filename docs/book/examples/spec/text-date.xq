(: The date of the draft, written in English, German and French. The
   date element has the year, the month's name and the day. :)
let $date := /rfc/front/date
let $months := ("January", "February", "March", "April", "May", "June", "July",
                "August", "September", "October", "November", "December")
let $month := index-of($months, $date/@month)
let $value := xs:date(`{$date/@year}-{format-integer($month, "01")}-{format-integer($date/@day, "01")}`)
return (
  format-date($value, "[D] [MNn] [Y]", "en", (), ()),
  format-date($value, "[FNn], [D]. [MNn] [Y]", "de", (), ()),
  format-date($value, "[Fn] [D] [Mn] [Y]", "fr", (), ())
)
