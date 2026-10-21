(:~ Parses the generated TypeScript and checks that its parts fit together. :)
declare variable $directory as xs:string external := "generated";

declare function local:parse($file as xs:string) as document-node() {
  sxml:parse-code(unparsed-text($file), "typescript")
};

let $program := doc($directory || "/program.xml")/program
let $files := $program/module/@file ! string()
let $code := map:merge($files ! { .: local:parse($directory || "/" || .) })
let $clients := $files[not(. = ("types.ts", "server.ts"))]
let $runtime := local:parse("runtime.ts")

let $functions := function($file) {
  $code($file)//export_statement/function_declaration/identifier ! string()
}
let $methods := $code("server.ts")//interface_declaration[type_identifier = "Implementation"]
  //method_signature/property_identifier ! string()
let $routes := $code("server.ts")//call_expression[identifier = "serve"]
  /arguments/object/pair/property_identifier ! string()
let $exports := $runtime//export_statement ! (.//*[@field = "name"])[1] ! string()

let $problems := (
  for $file in $clients
  return (
    $program/module[@file = $file]/function/@name[not(. = $functions($file))]
      ! `{ $file } lacks the function { . } of the tree`,
    $functions($file)[not(. = $methods)]
      ! `{ $file }: client function { . } has no method in Implementation`
  ),
  $methods[not(. = $clients ! $functions(.))] ! `method { . } is in no client`,
  $methods[not(. = $routes)] ! `method { . } has no route in createHandler`,
  for $file in $files
  for $name in $code($file)
    //import_statement[string/string_fragment = "../runtime.ts"]//import_specifier/identifier
  where not($name = $exports)
  return `{ $file } imports { $name }, which runtime.ts doesn't export`
)
return
  if (empty($problems))
  then `{ count($methods) } functions in { count($clients) } clients: clients, server and runtime agree`
  else error(#local:mismatch, string-join($problems, "; "))
