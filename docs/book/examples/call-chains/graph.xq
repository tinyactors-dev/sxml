(: The call graph of the TypeScript files in a directory, as XML: one
   version of the flag service, before or after. :)
import module namespace graph = "urn:example:graph" at "graph.xqm";

declare option output:indent "yes";

declare variable $directory as xs:string external := "before";

graph:graph(graph:read($directory))
