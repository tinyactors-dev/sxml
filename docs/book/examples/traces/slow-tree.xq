(: Rebuilds the trees of a flat list of spans: each span's children
   are found by searching all the spans. :)
declare function local:tree($spans as element(span)*, $span as element(span)) as element(span) {
  <span>{ $span/@*, $spans[@parent = $span/@id] ! local:tree($spans, .) }</span>
};

let $spans := /spans/span
let $trees := $spans[empty(@parent)] ! local:tree($spans, .)
return count($trees/descendant-or-self::span)
