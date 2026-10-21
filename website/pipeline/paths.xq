<paths>{
  for $request in //request
  group by $path := string($request/@path)
  order by count($request) descending
  return
    <path name="{$path}"
          requests="{count($request)}"
          errors="{count($request[@status >= 400])}"
          slowest="{max($request/@ms ! xs:integer(.))}"/>
}</paths>
