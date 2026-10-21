(: Writes an archive of $count made-up requests to the old version of the
   Vane service, one every 100 ms from $start, in the normal form ($form
   "tree"), or as one flat list of spans that name their parents ($form
   "flat"). Two of every three requests are the backend SDK's
   GET /api/getFlags, the third the console's POST /api/killFlag. :)
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
    $peer ! attribute peer { . },
    $peer[. = ("postgresql", "redis")] ! attribute db { . },
    $children
  }</span>
};

(: The spans of request $trace, whose database query takes $query ms. :)
declare function local:request(
  $trace as xs:integer, $route as xs:string, $query as xs:decimal
) as element(span) {
  if ($route = "GET /api/getFlags") then
    local:span(`{ $trace }.1`, "storefront", $route, "client", 0, $query + 12, "vane",
      local:span(`{ $trace }.2`, "vane", $route, "server", 1, $query + 10, (),
        local:span(`{ $trace }.3`, "vane", "flags.listFlags", "internal", 2, $query + 7, (),
          local:span(`{ $trace }.4`, "vane", "registry.getFlags", "internal", 3, $query + 4, (),
            local:span(`{ $trace }.5`, "vane", "SELECT flags", "client", 5, $query, "postgresql", ())))))
  else
    local:span(`{ $trace }.1`, "vane-console", $route, "client", 0, $query + 23, "vane.example.com",
      local:span(`{ $trace }.2`, "vane", $route, "server", 1, $query + 21, (),
        local:span(`{ $trace }.3`, "vane", "flags.killFlag", "internal", 2, $query + 19, (), (
          local:span(`{ $trace }.4`, "vane", "cache.getFlag", "internal", 3, 3, (),
            local:span(`{ $trace }.5`, "vane", "GET", "client", 4, 2, "redis", ())),
          local:span(`{ $trace }.6`, "vane", "registry.getFlag", "internal", 7, $query + 3, (),
            local:span(`{ $trace }.7`, "vane", "SELECT flags", "client", 8, $query, "postgresql", ())),
          local:span(`{ $trace }.8`, "vane", "registry.setPercentage", "internal", $query + 11, 8, (),
            local:span(`{ $trace }.9`, "vane", "UPDATE flags", "client", $query + 12, 6, "postgresql", ()))
        ))))
};

let $traces :=
  for $trace in 1 to xs:integer($count)
  let $route := if ($trace mod 3 = 0) then "POST /api/killFlag" else "GET /api/getFlags"
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
    <service name="storefront" version="5.2.0"/>
    <service name="vane" version="2.3.0"/>
    <service name="vane-console" version="1.8.0"/>
    { $traces }
  </traces>
