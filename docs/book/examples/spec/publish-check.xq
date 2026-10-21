(: Checks a draft before it is published. Problems that make the
   published text wrong are errors and stop publication; open editorial
   matters are warnings. :)
declare namespace spec = "urn:example:spec";
declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $draft external := "vane.xml";
declare variable $rfc := doc($draft)/rfc;

(: The key words of BCP 14, as a regular expression of whole words. :)
declare variable $keywords :=
  "(^|\W)(MUST|SHALL|REQUIRED|SHOULD|RECOMMENDED|MAY|OPTIONAL)(\W|$)";

let $anchors := $rfc//@anchor
let $errors := (
  for $xref in $rfc//xref[not(@target = $anchors)]
  return `{$xref/ancestor::section[1]/@anchor}: xref to a missing anchor "{$xref/@target}"`,
  for $reference in $rfc//reference[not(@anchor = $rfc//xref/@target)]
  return `references: [{$reference/@anchor}] is never cited`,
  for $text in $rfc/middle//t//text()[not(ancestor::bcp14)]
  where not($text/ancestor::section/@anchor = "terminology")
  where matches($text, $keywords)
  return `{$text/ancestor::section[1]/@anchor}: a key word without bcp14: "{normalize-space($text)}"`
)
let $warnings := (
  for $t in $rfc//t[bcp14][not(@anchor)]
  return `{$t/ancestor::section[1]/@anchor}: a requirement without an anchor`,
  for $cref in $rfc//cref
  return `{$cref/ancestor::section[1]/@anchor}: open note {$cref/@anchor} ({$cref/@source})`
)
return if (exists($errors))
  then error(xs:QName("spec:check"), string-join($errors, "; "))
  else $warnings ! `warning: {.}`
