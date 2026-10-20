declare option output:indent "yes";

let $loans := doc("loans.xml")//loan
return
  <members>{
    for $loan in $loans
    group by $member := string($loan/@member)
    order by count($loan) descending, $member
    return
      <member name="{ $member }" loans="{ count($loan) }">{
        for $id in distinct-values($loan/@book)
        return <book>{ string(//book[@id = $id]/title) }</book>
      }</member>
  }</members>
