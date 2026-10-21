(:~ Checks a draft for the mistakes its schema cannot see. It writes a
    line for each problem and fails with the error spec:check when it
    finds any, so that a script stops. :)
import module namespace spec = "urn:example:spec" at "spec.xqm";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $anchors := spec:anchor-map(/);

(: The paragraph of BCP 14's boilerplate quotes the keywords on purpose. :)
declare variable $boilerplate := //t[xref/@target = "RFC2119"];

declare variable $problems := (
  for $xref in //xref[not(map:contains($anchors, @target))]
  return `{$xref/ancestor::*[@anchor][1]/@anchor}: a cross reference to the missing anchor {$xref/@target}`,

  for $defined in //(dt | reference)[@anchor]
  where empty(//xref[@target = $defined/@anchor])
  return `{$defined/@anchor}: {if ($defined/self::dt) then "a term" else "a reference"} that nothing refers to`,

  for $text in //t[not(. is $boilerplate)]//text()[not(ancestor::bcp14 | ancestor::cref)]
  for $word in tokenize($text, "[^A-Z]+")[. = $spec:keywords]
  return `{$text/ancestor::*[@anchor][1]/@anchor}: {$word} without bcp14 markup`,

  for $paragraph in //t[bcp14][not(@anchor)]
  return `{$paragraph/ancestor::section[1]/@anchor}: a requirement without an anchor ({$paragraph/bcp14})`
);

if (empty($problems))
then `{/rfc/@docName}: no problems`
else (
  (: fn:message writes each problem on standard error, before the error. :)
  $problems ! message(text { . }),
  error(xs:QName("spec:check"), `{count($problems)} problem(s) in {/rfc/@docName}`)
)
