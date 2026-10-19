(: XMark Q11 (QT4 app-XMark XMark-Q11): a theta join (persons by income
   against every initial price), quadratic by its own text. :)
<XMark-result-Q11> {
  let $auction := (/) return
  for $p in $auction/site/people/person
  let $l := for $i in $auction/site/open_auctions/open_auction/initial
            where $p/profile/@income > 5000 * exactly-one($i/text())
            return $i
  return <items name="{$p/name/text()}">{count($l)}</items>
} </XMark-result-Q11>
