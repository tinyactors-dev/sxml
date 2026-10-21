(: Time per service and operation, from a CSV export of one trace,
   written as CSV. :)
declare option output:method "csv";

let $spans := parse-csv(unparsed-text("spans.csv"), { "header": true() })
let $column := $spans?column-index
return (
  ["service", "operation", "calls", "total ms"],
  for $row in $spans?rows
  group by $service := $row($column?Service),
           $operation := $row($column?Operation)
  order by $service, $operation
  return [
    $service,
    $operation,
    count($row),
    sum($row ! xs:decimal(.($column?("Duration (ms)"))))
  ]
)
