(: An OTLP JSON file with only the traces of one route. :)
declare option output:method "json";
declare option output:indent "yes";
declare option output:escape-solidus "no";

declare variable $file external := "before.json";
declare variable $route external;

let $request := json-doc($file)
let $roots := $request?resourceSpans?*?scopeSpans?*?spans?*[empty(?parentSpanId)]
let $traces := $roots[?name = $route]?traceId
return map:put($request, "resourceSpans", array {
  for member $resource in $request?resourceSpans
  let $scopes := array {
    for member $scope in $resource?scopeSpans
    let $spans := array:filter($scope?spans, fn { ?traceId = $traces })
    where array:size($spans) > 0
    return map:put($scope, "spans", $spans)
  }
  where array:size($scopes) > 0
  return map:put($resource, "scopeSpans", $scopes)
})
