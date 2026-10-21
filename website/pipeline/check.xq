declare variable $rules := doc("rules.xml")/rules/*;

<violations>{
  for $path in sort(file:list("src", true(), "*.ts"))
  let $file := "src/" || $path
  let $code := sxml:parse-code(unparsed-text($file),
                 "typescript", { "positions": true() })
  for $rule in $rules
  return switch (local-name($rule))
    case "max-parameters" return
      for $function in $code//function_declaration
      let $parameters := $function/formal_parameters/(
        required_parameter | optional_parameter)
      where count($parameters) > xs:integer($rule/@limit)
      return <violation file="{$file}"
               line="{$function/@line}"
               name="{$function/identifier}"
               count="{count($parameters)}">{$rule}</violation>
    case "only-calls" case "no-calls" return
      let $inside := starts-with($file, $rule/@directory)
      for $call in $code//call_expression
      where $call/*[@field = "function"] = $rule/@function
        and $inside = ($rule instance of element(no-calls))
      return <violation file="{$file}"
               line="{$call/@line}">{$rule}</violation>
    default return ()
}</violations>
