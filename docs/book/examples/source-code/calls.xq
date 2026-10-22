(: Every call of the function $name in the JavaScript files, with its line. :)
declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $name external := "formatPrice";

for $file in ("cart.js", "money.js")
let $code := tree-sitter:parse(unparsed-text($file), "javascript", { "positions": true() })
for $call in $code//call_expression[*[@field = "function"] = $name]
return `{$file}:{$call/@line}: {$call}`
