(: Revenue by product, from a CSV file, written as CSV. :)
declare option output:method "csv";

let $sales := parse-csv(unparsed-text("sales.csv"), { "header": true() })
let $column := $sales?column-index
return (
  ["product", "units", "revenue"],
  for $row in $sales?rows
  group by $product := $row($column?product)
  order by $product
  return [
    $product,
    sum($row ! xs:integer(.($column?units))),
    sum($row ! (xs:integer(.($column?units)) * xs:decimal(.($column?price))))
  ]
)
