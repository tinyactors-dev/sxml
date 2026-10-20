declare option output:indent "yes";

<titles>{
  for $book in //book
  order by xs:integer($book/@year)
  return <title year="{ $book/@year }">{ string($book/title) }</title>
}</titles>
