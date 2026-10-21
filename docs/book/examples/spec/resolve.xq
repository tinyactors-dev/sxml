declare namespace spec = "urn:example:spec";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $anchored := //*[@anchor];

(:~ The text a cross reference to the anchor shows, or the error
    spec:dangling when no element has the anchor. :)
declare function local:xref-text($target as xs:string) as xs:string {
  let $element := $anchored[@anchor = $target]
    otherwise error(xs:QName("spec:dangling"), "No element has the anchor " || $target, $target)
  return typeswitch ($element)
    case $section as element(section) return `Section "{$section/name}"`
    case element(reference) return `[{$target}]`
    case $term as element(dt) return `the term "{$term}"`
    default return $target
};

for $xref in //xref
let $where := $xref/ancestor::*[@anchor][1]/@anchor
return
  try {
    `{$where}: {local:xref-text($xref/@target)}`
  } catch spec:dangling {
    `{$where}: {$err:description}`
  }
