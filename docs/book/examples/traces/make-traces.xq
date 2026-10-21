(: Writes an archive of $count made-up requests to the old flag service,
   one every 100 ms from $start, in the normal form ($form "tree"), or as
   one flat list of spans that name their parents ($form "flat"). :)
declare variable $count external := 1000;
declare variable $start external := "2026-10-21T09:00:00Z";
declare variable $form external := "tree";

declare function local:span(
  $id as xs:string, $service as xs:string, $name as xs:string,
  $kind as xs:string, $start as xs:decimal, $duration as xs:decimal,
  $peer as xs:string?, $children as element(span)*
) as element(span) {
  <span id="{ $id }" service="{ $service }" name="{ $name }" kind="{ $kind }"
        start="{ $start }" duration="{ $duration }">{
    $peer ! attribute peer { . }, $children
  }</span>
};

(: The spans of request $trace, whose database query takes $query ms. :)
declare function local:request(
  $trace as xs:integer, $route as xs:string, $query as xs:decimal
) as element(span) {
  local:span(`{ $trace }.1`, "api-gateway", $route, "server", 0, $query + 16, (), (
    local:span(`{ $trace }.2`, "api-gateway", $route, "client", 1, $query + 14, "flag-service", (
      local:span(`{ $trace }.3`, "flag-service", $route, "server", 2, $query + 12, (),
        if ($route = "GET /flags") then
          local:span(`{ $trace }.4`, "flag-service", "GET /flags", "client", 4, $query + 8, "flag-registry",
            local:span(`{ $trace }.5`, "flag-registry", "GET /flags", "server", 5, $query + 6, (),
              local:span(`{ $trace }.6`, "flag-registry", "SELECT flags", "client", 7, $query, "postgresql", ())))
        else (
          local:span(`{ $trace }.4`, "flag-service", "GET", "client", 3, 2, "flag-cache", ()),
          local:span(`{ $trace }.5`, "flag-service", "GET /flags/{key}", "client", 6, $query + 4, "flag-registry",
            local:span(`{ $trace }.6`, "flag-registry", "GET /flags/{key}", "server", 7, $query + 2, (),
              local:span(`{ $trace }.7`, "flag-registry", "SELECT flags", "client", 8, $query, "postgresql", ())))
        )
      )
    ))
  ))
};

let $traces :=
  for $trace in 1 to xs:integer($count)
  let $route := if ($trace mod 3 = 0) then "POST /flags/{key}/kill" else "GET /flags"
  let $at := xs:dateTime($start) + xs:dayTimeDuration("PT0.1S") * ($trace - 1)
  return <trace id="{ $trace }" route="{ $route }" start="{ $at }">{
    local:request($trace, $route, 5 + $trace * 7919 mod 61)
  }</trace>
return if ($form = "flat") then
  <spans>{
    for $span in $traces//span
    return <span>{ $span/parent::span/@id ! attribute parent { . }, $span/@* }</span>
  }</spans>
else
  <traces source="make-traces.xq">
    <service name="api-gateway" version="1.4.0"/>
    <service name="flag-service" version="2.3.0"/>
    <service name="flag-registry" version="1.9.2"/>
    { $traces }
  </traces>
