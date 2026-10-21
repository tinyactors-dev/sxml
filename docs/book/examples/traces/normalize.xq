(: An OTLP JSON file in the normal form: a tree of spans per trace. :)
import module namespace trace = "urn:example:trace" at "trace.xqm";
declare option output:indent "yes";

declare variable $file external := "before.json";

trace:load($file)
