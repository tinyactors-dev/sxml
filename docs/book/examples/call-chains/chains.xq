(: The call chains of the TypeScript files in a directory, one per line:
   one version of the Vane service, before or after. The chains are made
   with a worklist, or with --param=method=recursive by recursion. :)
import module namespace graph = "urn:example:graph" at "graph.xqm";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $directory as xs:string external := "before";
declare variable $method as xs:string external := "worklist";

let $graph := graph:graph(graph:read($directory))
let $chains := if ($method = "recursive") then graph:chains-recursive($graph)
               else graph:chains($graph)
return $chains ! graph:chain-text(.)
