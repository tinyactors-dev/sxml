(: What changed between two trace files, as XML for the report. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:indent "yes";

declare variable $before external := "before.json";
declare variable $after external := "after.json";

let $old := trace:load($before)
let $new := trace:load($after)
return <diff before="{ $before }" after="{ $after }">{
  for $version in trace:versions($old, $new)
  return <version service="{ $version?service }">{
    $version?before ! attribute before { . },
    $version?after ! attribute after { . }
  }</version>,
  for $change in trace:diff($old, $new)
  return <change route="{ $change?route }" caller="{ $change?caller }"
                 callee="{ $change?callee }" before="{ $change?before }"
                 after="{ $change?after }"/>
}</diff>
