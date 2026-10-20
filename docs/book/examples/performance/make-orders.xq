(: Writes a document with $count orders. :)
declare variable $count external := 1000;

<orders>{
  for $id in 1 to xs:integer($count)
  return
    <order id="{ $id }">
      <customer>c{ $id mod 7 }</customer>
      <total>{ $id mod 1000 div 10 }</total>
    </order>
}</orders>
