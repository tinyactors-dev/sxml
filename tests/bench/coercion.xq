(: Coercion of function arguments (packet perf-8): built-in functions given
   untyped attribute values where they require xs:string? (atomization and
   a cast to xs:string), a user function with typed parameters (a node, a
   string from an attribute, an integer promoted to xs:double), and
   arguments that already have the required type. 2,000 elements, read
   twenty times; over a constructed document, so no file is parsed. :)
declare function local:label($name as xs:string, $element as element(),
                             $weight as xs:double) as xs:string {
  concat($name, "/", local-name($element), "/", $weight)
};
let $document := document {
  <items>{
    for $index in 1 to 2000
    return <item id="i{$index}" name="name-{$index}" kind="{$index mod 7}"/>
  }</items>
}
let $items := $document/items/item
return (
  count(for $round in 1 to 20, $item in $items
        return concat($item/@id, "-", $item/@name)),
  sum(for $round in 1 to 20, $item in $items
      return string-length(replace($item/@name, "-", "_"))),
  count(for $round in 1 to 20, $item in $items
        return local:label($item/@name, $item, $round)),
  sum(for $round in 1 to 20, $item in $items
      return string-length(substring-after(string($item/@name), "-")))
)
