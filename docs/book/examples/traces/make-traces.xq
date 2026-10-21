(: Writes an archive of $count made-up requests to the old lock service,
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
    local:span(`{ $trace }.2`, "api-gateway", $route, "client", 1, $query + 14, "lock-service", (
      local:span(`{ $trace }.3`, "lock-service", $route, "server", 2, $query + 12, (),
        if ($route = "GET /doors") then
          local:span(`{ $trace }.4`, "lock-service", "GET /doors", "client", 4, $query + 8, "lock-registry",
            local:span(`{ $trace }.5`, "lock-registry", "GET /doors", "server", 5, $query + 6, (),
              local:span(`{ $trace }.6`, "lock-registry", "SELECT doors", "client", 7, $query, "postgresql", ())))
        else (
          local:span(`{ $trace }.4`, "lock-service", "GET", "client", 3, 2, "door-cache", ()),
          local:span(`{ $trace }.5`, "lock-service", "GET /doors/{id}", "client", 6, $query + 4, "lock-registry",
            local:span(`{ $trace }.6`, "lock-registry", "GET /doors/{id}", "server", 7, $query + 2, (),
              local:span(`{ $trace }.7`, "lock-registry", "SELECT doors", "client", 8, $query, "postgresql", ())))
        )
      )
    ))
  ))
};

let $traces :=
  for $trace in 1 to xs:integer($count)
  let $route := if ($trace mod 3 = 0) then "POST /doors/{id}/unlock" else "GET /doors"
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
    <service name="lock-service" version="2.3.0"/>
    <service name="lock-registry" version="1.9.2"/>
    { $traces }
  </traces>
