(: The spans of a trace file as CSV, one row per span, for a spreadsheet. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:method "csv";

declare variable $file external := "before.json";

["route", "service", "name", "kind", "start", "duration"],
for $span in trace:load($file)//span
return [
  string($span/ancestor::trace/@route),
  string($span/@service), string($span/@name), string($span/@kind),
  xs:decimal($span/@start), xs:decimal($span/@duration)
]
