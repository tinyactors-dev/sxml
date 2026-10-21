(: How often each service calls another, per request of each route. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $file external := "before.json";

let $cardinality := trace:cardinality(trace:load($file))
for $key in map:keys($cardinality)
order by $key
return `{ $key }  { $cardinality($key)?count }`
