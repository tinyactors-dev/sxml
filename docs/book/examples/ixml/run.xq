(: Runs a program of the small language of calc.ixml, in the file $file:
   each print statement gives one integer. :)
declare variable $file external;

declare function local:value($node as element(), $names as map(*)) as xs:integer {
  typeswitch ($node)
    case element(number) return xs:integer($node)
    case element(variable) return $names(string($node/@name))
    default return
      let $left := local:value($node/*[1], $names)
      let $right := local:value($node/*[2], $names)
      return switch (string($node/@op))
        case "+" return $left + $right
        case "-" return $left - $right
        case "*" return $left * $right
        default return $left idiv $right
};

declare function local:run($statements as element()*, $names as map(*)) as xs:integer* {
  if (empty($statements)) then () else
  let $statement := head($statements)
  return
    if ($statement instance of element(let)) then
      local:run(tail($statements),
        map:put($names, string($statement/@name), local:value($statement/*, $names)))
    else (
      local:value($statement/*, $names),
      local:run(tail($statements), $names)
    )
};

let $program := invisible-xml(unparsed-text("calc.ixml"))(unparsed-text($file))
return local:run($program/program/*, {})
