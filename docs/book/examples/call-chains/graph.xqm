(: The call graph of a TypeScript program: its functions, and for each
   call in a function, the function it calls. :)
module namespace graph = "urn:example:graph";

declare namespace file = "http://expath.org/ns/file";
declare namespace tree-sitter = "urn:sxml:tree-sitter";

(: What a call calls: the id of a function, and whether that function is
   outside the program. :)
declare record graph:target(id as xs:string, external as xs:boolean);

(: ANCHOR: functions :)
(: A function with a name of its own: a function declaration, a method,
   or an arrow function or function expression assigned to a const. An
   arrow function passed as an argument has no name, and its calls belong
   to the function around it. :)
declare function graph:is-function($node as element()) as xs:boolean {
  exists($node[self::function_declaration or self::method_definition])
  or exists($node[self::arrow_function or self::function_expression]
                 [parent::variable_declarator])
};

(: The function a node is in. :)
declare function graph:function-of($node as node()) as element()? {
  $node/ancestor::*[graph:is-function(.)][1]
};

(: The name of a function; a method is named after its class. :)
declare function graph:function-name($function as element()) as xs:string {
  typeswitch ($function)
    case element(function_declaration)
      return string($function/*[@field = "name"])
    case element(method_definition)
      return $function/ancestor::class_declaration/type_identifier
             || "." || $function/*[@field = "name"]
    default
      return string($function/parent::variable_declarator/*[@field = "name"])
};
(: ANCHOR_END: functions :)

(: The module of a file is its name without the directory and ".ts". :)
declare function graph:module-name($path as xs:string) as xs:string {
  replace($path, "^(.*/)?([^/]+)\.ts$", "$2")
};

(: ANCHOR: imports :)
(: What the imported names of a module stand for: a namespace import
   names a module, a named import a function of a module. Imports from
   packages (node:http, redis, pg) are left out: calls to them are
   external. :)
declare function graph:imports($program as element(program)) as map(*) {
  map:merge(
    for $import in $program/import_statement
    let $source := string($import/string/string_fragment)
    where starts-with($source, "./")
    let $module := graph:module-name($source)
    return (
      $import//namespace_import/identifier ! { string(.): $module },
      $import//import_specifier/*[@field = "name"]
        ! { string(.): $module || "." || . }
    )
  )
};
(: ANCHOR_END: imports :)

(: ANCHOR: locals :)
(: The names a function declares: its parameters, variables and loop
   variables, and those of the anonymous functions inside it. :)
declare function graph:locals($function as element()) as xs:string* {
  $function//(required_parameter | optional_parameter)/*[@field = "pattern"],
  $function//variable_declarator/identifier[@field = "name"],
  $function//shorthand_property_identifier_pattern,
  $function//for_in_statement/identifier[@field = "left"]
};

(: The name a callee starts with: registry in registry.getFlag, flag in
   flag.owners.includes, this in this.check; empty when it starts with a
   value, as new Date().toISOString does. :)
declare function graph:root($callee as element()) as element()? {
  if ($callee instance of element(member_expression))
  then graph:root($callee/*[@field = "object"])
  else $callee[self::identifier or self::this]
};
(: ANCHOR_END: locals :)

(: ANCHOR: callee :)
(: The function a call calls, or nothing for a method of a local value.
   $module is the caller's module, $functions the ids of all functions
   of the program. :)
declare function graph:callee(
  $call as element(call_expression),
  $module as xs:string,
  $functions as xs:string*
) as graph:target? {
  let $callee := $call/*[@field = "function"]
  let $imports := graph:imports($call/ancestor::program)
  let $locals := graph:locals(graph:function-of($call))
  let $root := graph:root($callee)
  return
    if ($callee instance of element(identifier)) then (
      if (map:contains($imports, $callee))
      then graph:target($imports($callee), false())
      else if ($functions = $module || "." || $callee)
      then graph:target($module || "." || $callee, false())
      else if ($locals = $callee) then ()
      else graph:target($callee, true())
    )
    else if ($root instance of element(this))
    then graph:target($module || "." || $callee/ancestor::class_declaration/type_identifier
                      || "." || $callee/*[@field = "property"], false())
    else if ($callee/*[@field = "object"] is $root
             and map:contains($imports, $root))
    then graph:target($imports($root) || "." || $callee/*[@field = "property"],
                      false())
    else if (empty($root) or $locals = $root) then ()
    else graph:target($callee, true())
};
(: ANCHOR_END: callee :)

(: ANCHOR: in-loop :)
(: Whether a node is in a loop of its own function. :)
declare function graph:in-loop($node as node()) as xs:boolean {
  some $loop in $node/ancestor::*[self::for_statement or self::for_in_statement
                                  or self::while_statement or self::do_statement]
  satisfies graph:function-of($loop) is graph:function-of($node)
};
(: ANCHOR_END: in-loop :)

(: ANCHOR: graph :)
(: The call graph of a program, given as its modules by name. :)
declare function graph:graph(
  $modules as map(xs:string, element(program))
) as element(graph) {
  let $functions :=
    for $module in sort(map:keys($modules))
    for $function in $modules($module)//*[graph:is-function(.)]
    return { "module": $module, "element": $function,
             "id": $module || "." || graph:function-name($function) }
  return <graph>{
    for $function in $functions
    return <function id="{ $function?id }" line="{ $function?element/@line }"/>,
    for $function in $functions
    for $call in $function?element//call_expression
    where graph:function-of($call) is $function?element
    let $target := graph:callee($call, $function?module, $functions?id)
    (: A function of the module passed as an argument is called from
       here too, by the function it is passed to. :)
    let $passed := $call/arguments/identifier
                   [$functions?id = $function?module || "." || .]
                   ! graph:target($function?module || "." || ., false())
    for $to at $position in ($target, $passed)
    return <call from="{ $function?id }" to="{ $to?id }" line="{ $call/@line }">{
      if (graph:in-loop($call)) { attribute repeated { "true" } },
      if ($to?external) { attribute external { "true" } },
      if ($position > count($target)) { attribute passed { "true" } }
    }</call>
  }</graph>
};
(: ANCHOR_END: graph :)

(: ANCHOR: read :)
(: The modules of a directory, parsed. :)
declare function graph:read(
  $directory as xs:string
) as map(xs:string, element(program)) {
  map:merge(
    for $file in file:list($directory, false(), "*.ts")
    return { graph:module-name($file):
               tree-sitter:parse(file:read-text($directory || "/" || $file),
                               "typescript", { "positions": true() })/program }
  )
};
(: ANCHOR_END: read :)

(: ANCHOR: next :)
(: The functions no call reaches: where the program is entered. :)
declare function graph:entries($graph as element(graph)) as xs:string* {
  $graph/function[not(@id = $graph/call/@to)]/@id
};

(: The calls a chain can go on with: the calls of its last function to
   functions of the program that the chain has not been through (so a
   cycle ends a chain). A chain is a sequence of calls. :)
declare function graph:next(
  $graph as element(graph),
  $chain as element(call)*
) as element(call)* {
  $graph/call[@from = $chain[last()]/@to][not(@external)]
             [not(@to = $chain/@from)]
};
(: ANCHOR_END: next :)

(: ANCHOR: chains :)
(: Every chain of calls from an entry to a function that calls no other
   function of the program, made with a worklist: each step takes the
   first chain off the list, and either keeps it, when it cannot go on,
   or puts back one longer chain per next call. :)
declare function graph:chains($graph as element(graph)) as element(chain)* {
  let $start := {
    "work": array {
      for $entry in graph:entries($graph)
      for $call in $graph/call[@from = $entry][not(@external)]
      return <chain>{ $call }</chain>
    },
    "done": ()
  }
  let $end := while-do($start, fn { array:size(?work) > 0 }, fn($state) {
    let $chain := array:head($state?work)
    let $next := graph:next($graph, $chain/call)
    return {
      "work": array:join((
        array { $next ! <chain>{ $chain/call, . }</chain> },
        array:tail($state?work))),
      "done": ($state?done, $chain[empty($next)])
    }
  })
  return $end?done
};
(: ANCHOR_END: chains :)

(: ANCHOR: recursive :)
(: The same chains, by a function that calls itself: the chains that
   go on from a chain are those that go on from it and each next call. :)
declare function graph:extend(
  $graph as element(graph),
  $chain as element(call)*
) as element(chain)* {
  let $next := graph:next($graph, $chain)
  return if (empty($next)) then <chain>{ $chain }</chain>
         else $next ! graph:extend($graph, ($chain, .))
};

declare function graph:chains-recursive($graph as element(graph)) as element(chain)* {
  for $entry in graph:entries($graph)
  for $call in $graph/call[@from = $entry][not(@external)]
  return graph:extend($graph, $call)
};
(: ANCHOR_END: recursive :)

(: ANCHOR: chain-text :)
(: A chain as text: the functions it goes through, a star after each
   one called in a loop. :)
declare function graph:chain-text($chain as element(chain)) as xs:string {
  string-join(($chain/call[1]/@from,
               $chain/call ! (@to || (if (@repeated) then "*" else ""))),
              " > ")
};
(: ANCHOR_END: chain-text :)
