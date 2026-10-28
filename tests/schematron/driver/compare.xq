(: Compares what sxml did with a case of the Schematron suite with its
   expected result (tests/schematron/README.md, "The format"), as
   tests/schematron/tools/crosscheck.py compares SchXslt's: the findings
   as multisets of (kind, location, id, role, flag, severity, document,
   message, diagnostics, properties), the flags as a set, each expected
   error as a failure of the right stage. The driver appends a call of
   local:compare with the SVRL document (when the case ran) as its last
   argument. The result is the verdict, "pass", "implementation-defined: ..."
   or "fail: ...", then, after a pass, each assert-svrl expression for the
   driver to evaluate over the SVRL. :)
declare namespace t = "urn:sxml:schematron-test-suite";
declare namespace svrl = "http://purl.oclc.org/dsdl/svrl";

declare variable $local:separator := " | ";

(: 5.3: whitespace is U+0020, U+0009, U+000D and U+000A, as normalize-space
   takes it. :)
declare function local:text($node as node()?) as xs:string {
  normalize-space(string($node))
};

declare function local:key($kind as xs:string, $location as xs:string?,
                           $fields as xs:string*) as xs:string {
  string-join(($kind, ($location, "*")[1], $fields), $local:separator)
};

declare function local:expected($result as element(t:result), $compares-lang as xs:boolean)
    as xs:string* {
  for $finding in $result/t:findings/t:finding
  return local:key($finding/@kind, $finding/@location, (
    for $name in ("id", "role", "flag", "severity", "document")
    return $name || "=" || $finding/@*[local-name() = $name],
    local:text($finding/t:message),
    sort(for $diagnostic in $finding/t:diagnostic
         return string-join(($diagnostic/@ref, $diagnostic/@role,
                             if ($compares-lang) then $diagnostic/@lang else (),
                             local:text($diagnostic)), "/")) => string-join(", "),
    sort(for $property in $finding/t:property
         return string-join(($property/@ref, $property/@role, $property/@scheme,
                             local:text($property)), "/")) => string-join(", ")))
};

(: The document of a finding: that of the fired rule before it in its
   pattern, else the one document its active pattern names; none for the
   instance itself. :)
declare function local:document($finding as element(), $instance as xs:string,
                                $directory as xs:string) as xs:string? {
  let $pattern := $finding/preceding-sibling::svrl:active-pattern[1]
  let $rule := $finding/preceding-sibling::svrl:fired-rule[1][. >> $pattern or empty($pattern)]
  let $documents := if ($rule/@document) then string($rule/@document)
                    else tokenize($pattern/@documents)
  return if (count($documents) = 1 and $documents != $instance)
         then substring-after($documents, $directory) else ()
};

declare function local:actual($svrl as document-node()?, $compares-lang as xs:boolean,
                              $instance as xs:string, $directory as xs:string) as xs:string* {
  for $finding in $svrl/svrl:schematron-output/(svrl:failed-assert | svrl:successful-report)
  let $document := local:document($finding, $instance, $directory)
  return local:key(local-name($finding), $finding/@location, (
    for $name in ("id", "role", "flag", "severity")
    return $name || "=" || $finding/@*[local-name() = $name],
    "document=" || $document,
    local:text($finding/svrl:text),
    sort(for $diagnostic in $finding/svrl:diagnostic-reference
         return string-join(($diagnostic/@diagnostic, $diagnostic/@role,
                             if ($compares-lang) then ($diagnostic/@xml:lang, $diagnostic/svrl:text/@xml:lang)[1] else (),
                             local:text($diagnostic/svrl:text)), "/")) => string-join(", "),
    sort(for $property in $finding/svrl:property-reference
         return string-join(($property/@property, $property/@role, $property/@scheme,
                             local:text($property/svrl:text)), "/")) => string-join(", ")))
};

(: Expected keys without a location match an actual key whatever its
   location. :)
declare function local:matches($expected as xs:string, $actual as xs:string) as xs:boolean {
  let $expected-parts := tokenize($expected, " \| ")
  let $actual-parts := tokenize($actual, " \| ")
  return if ($expected-parts[2] = "*")
         then deep-equal($expected-parts[position() != 2], $actual-parts[position() != 2])
         else $expected = $actual
};

declare function local:compare-findings($expected as xs:string*, $actual as xs:string*)
    as xs:string* {
  let $state := fold-left($expected, {"unmatched": array { $actual }, "missing": []},
    function($state, $key) {
      let $unmatched := $state?unmatched
      let $index := (for $member at $position in $unmatched?*
                     where local:matches($key, $member) return $position)[1]
      return if (empty($index))
             then map:put($state, "missing", array:append($state?missing, $key))
             else map:put($state, "unmatched", array:remove($unmatched, $index))
    })
  return (
    if (array:size($state?missing) > 0)
    then "missing " || string-join($state?missing?*, " ; ") else (),
    if (array:size($state?unmatched) > 0)
    then "unexpected " || string-join($state?unmatched?*, " ; ") else ())
};

(: Whether an expected outcome other than findings is met. :)
declare function local:error-met($expected as element(), $outcome as xs:string) as xs:boolean {
  typeswitch ($expected)
    case element(t:schema-error) return $outcome = "compile-failed"
    case element(t:error) return $outcome = "validate-failed"
    default return false()
};

declare function local:findings-problems($result as element(t:result), $svrl as document-node()?,
                                         $instance as xs:string, $directory as xs:string)
    as xs:string* {
  let $compares-lang := exists($result//t:diagnostic/@lang)
  let $problems := local:compare-findings(local:expected($result, $compares-lang),
                                          local:actual($svrl, $compares-lang, $instance, $directory))
  let $flags := distinct-values($svrl/svrl:schematron-output/*/tokenize(@flag))
  let $expected-flags := $result/t:flags ! distinct-values(tokenize(.))
  return ($problems,
    if (exists($result/t:flags) and
        not(deep-equal(sort($flags), sort($expected-flags))))
    then "flags " || string-join(sort($flags), " ") || ", expected "
         || string-join(sort($expected-flags), " ")
    else ())
};

declare function local:compare($suite as xs:string, $set-name as xs:string,
                               $case-name as xs:string, $instance as xs:string,
                               $outcome as xs:string, $detail as xs:string,
                               $svrl as document-node()?) as xs:string* {
  let $test-set := doc($suite || "catalog.xml")/t:catalog/t:test-set[@name = $set-name]
  let $directory := $suite || $set-name || "/"
  let $case := doc($suite || $test-set/@file)/t:test-set/t:test-case[@name = $case-name]
  let $result := $case/t:result
  let $summary := if ($outcome = "ran")
    then (string-join(local:actual($svrl, false(), $instance, $directory), " ; "), "no findings")[.][1]
    else $outcome || ": " || $detail
  return
    if (exists($result/t:implementation-defined))
    then "implementation-defined: " || $summary
    else if (exists($result/t:findings)) then
      if ($outcome != "ran") then "fail: expected findings, " || $outcome || ": " || $detail
      else
        let $problems := local:findings-problems($result, $svrl, $instance, $directory)
        return if (exists($problems)) then "fail: " || string-join($problems, "; ")
               else ("pass", $result/t:assert-svrl ! string())
    else
      let $alternatives := ($result/t:any-of/*, $result/(t:schema-error | t:error))
      return if (some $alternative in $alternatives satisfies local:error-met($alternative, $outcome))
             then "pass"
             else "fail: expected " || string-join($alternatives ! local-name(), " or ")
                  || ", " || $summary
};
