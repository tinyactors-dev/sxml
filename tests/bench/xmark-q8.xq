(: XMark Q8 (QT4 app-XMark XMark-Q8): the names of persons and the number
   of items they bought; a join of person and closed_auction on a value
   equality in a where clause. :)
<XMark-result-Q8> {
  let $auction := (/) return
  for $p in $auction/site/people/person
  let $a := for $t in $auction/site/closed_auctions/closed_auction
            where $t/buyer/@person = $p/@id
            return $t
  return <item person="{$p/name/text()}">{count($a)}</item>
} </XMark-result-Q8>
