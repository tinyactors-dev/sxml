(:~ Parses the generated TypeScript and checks that its parts fit together. :)
declare variable $directory as xs:string external := "generated";

declare function local:parse($file as xs:string) as document-node() {
  sxml:parse-code(unparsed-text($file), "typescript")
};

let $program := doc($directory || "/program.xml")
let $client := local:parse($directory || "/client.ts")
let $server := local:parse($directory || "/server.ts")
let $runtime := local:parse("runtime.ts")

let $functions := $client//export_statement/function_declaration/identifier ! string()
let $methods := $server//interface_declaration[type_identifier = "Implementation"]
  //method_signature/property_identifier ! string()
let $routes := $server//call_expression[identifier = "serve"]
  /arguments/object/pair/property_identifier ! string()
let $exports := $runtime//export_statement ! (.//*[@field = "name"])[1] ! string()

let $problems := (
  $program/module[@file = "client.ts"]/function/@name[not(. = $functions)]
    ! `client.ts lacks the function {.} of the tree`,
  $functions[not(. = $methods)] ! `client function {.} has no method in Implementation`,
  $methods[not(. = $functions)] ! `method {.} has no client function`,
  $methods[not(. = $routes)] ! `method {.} has no route in createHandler`,
  for $file in ("types.ts", "client.ts", "server.ts")
  for $name in local:parse($directory || "/" || $file)
    //import_statement[string/string_fragment = "../runtime.ts"]//import_specifier/identifier
  where not($name = $exports)
  return `{ $file } imports { $name }, which runtime.ts doesn't export`
)
return
  if (empty($problems))
  then `{ count($functions) } functions: client, server and runtime agree`
  else error(#local:mismatch, string-join($problems, "; "))
