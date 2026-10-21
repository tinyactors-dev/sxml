(: The diff of two versions as a text report: what was added, removed
   and changed, and how often functions call other modules now. :)
declare option output:method "text";
declare option output:item-separator "&#10;";

declare function local:text($item as element()) as xs:string {
  typeswitch ($item)
    case element(call)
      return $item/@from || " > " || $item/@to
             || (if ($item/@repeated) then " (in a loop)" else "")
    case element(function) return string($item/@id)
    default return string($item)
};

for $kind in ("function", "call", "chain")
for $change in ("added", "removed", "changed")
let $items := /diff/*[local-name() = $kind][@change = $change]
where exists($items)
return (`{ $kind }s { $change }:`, $items ! ("  " || local:text(.)), ""),

"Calls to other modules, before and after:",
/diff/module ! `  {@from} > {@name}: {@old} -> {@new}`
