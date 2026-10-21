(: Rebuilds the trees of a flat list of spans: a map from each parent
   id to its children is made once, then each lookup is one step. :)
declare function local:tree($children as map(*), $span as element(span)) as element(span) {
  <span>{ $span/@*, $children($span/@id) ! local:tree($children, .) }</span>
};

let $spans := /spans/span
let $children := map:build($spans[@parent], fn { string(@parent) })
let $trees := $spans[empty(@parent)] ! local:tree($children, .)
return count($trees/descendant-or-self::span)
