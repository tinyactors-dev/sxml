xquery version "4.0";
(:
 : The driver of the iXML community test suite (third_party/ixml-tests,
 : D13, D215): runs the test sets of one catalog with fn:invisible-xml and
 : checks their results. tests/ixml/run-ixml-tests.sh runs it once per
 : catalog that third_party/ixml-tests/test-catalog.xml names, through
 : `sxml xquery`, so it uses only what any host of sxml's public API has.
 :
 : The catalog vocabulary (test-catalog.xml of the suite): a test set
 : names its grammar (ixml-grammar, ixml-grammar-ref, vxml-grammar,
 : vxml-grammar-ref; the nearest ancestor's when it names none), may have a
 : grammar-test (the grammar's own XML form, or that it is no grammar),
 : and has test cases, each an input (test-string, test-string-ref) and a
 : result: assert-xml or assert-xml-ref (one of them matches),
 : assert-not-a-sentence, assert-not-a-grammar, assert-dynamic-error. A
 : test with dependencies sxml does not meet (a Unicode version other than
 : sxml's 16.0) is not run.
 :
 : Comparison is fn:deep-equal of the expected element and the result's
 : document element, without an ixml:version attribute: the Specification
 : ("Prolog") asks for one when the grammar's version is not the one used,
 : and the suite's results predate that.
 :
 : Writes one line per test that did not pass ("fail", "error" or
 : "not-run", the catalog, the test set path and test name, and why),
 : then one line of counts:
 :   counts: CATALOG tests=N passed=P failed=F errors=E not-run=R
 :           grammar-tests=... grammar-passed=...
 :           correct-grammar-cases=... correct-grammar-passed=...
 : where a correct-grammar case is a test case whose grammar is expected
 : to be a grammar (the packet's gate, D215).
 :)
declare namespace tc = "http://invisiblexml.org/NS/test-catalog";
declare namespace ixml = "http://invisiblexml.org/NS";

declare option output:method "text";

declare variable $catalog external;

(: The Unicode version of sxml's tables (tools/generate-regex-tables.py). :)
declare variable $unicode-version := "16.0";

declare function local:href($element as element()) as xs:string {
  resolve-uri($element/@href, base-uri($element))
};

(: The grammar element of a test set or test case, or of the nearest
   ancestor that has one. :)
declare function local:grammar-spec($node as element()) as element()? {
  let $own := $node/(tc:ixml-grammar | tc:ixml-grammar-ref | tc:vxml-grammar
                     | tc:vxml-grammar-ref)
  return if (exists($own)) then head($own)
         else if ($node/parent::tc:test-set) then local:grammar-spec($node/parent::tc:test-set)
         else ()
};

(: The grammar of a grammar element: a string in ixml notation or an
   ixml element. :)
declare function local:grammar($spec as element()) as item() {
  typeswitch ($spec)
    case element(tc:ixml-grammar) return string($spec)
    case element(tc:ixml-grammar-ref) return unparsed-text(local:href($spec))
    case element(tc:vxml-grammar) return $spec/*[1]
    default return doc(local:href($spec))/*
};

(: A compiled parser, or the error that compiling raised. :)
declare function local:compile($spec as element()?) as map(*) {
  if (empty($spec)) then { "error": "no grammar" }
  else try {
    { "parser": invisible-xml(local:grammar($spec)) }
  } catch * {
    { "error": string($err:code) || ": " || $err:description, "code": $err:code }
  }
};

declare function local:input($case as element()) as xs:string {
  if ($case/tc:test-string-ref) then unparsed-text(local:href($case/tc:test-string-ref))
  else string($case/tc:test-string)
};

declare function local:without-version($element as element()) as element() {
  element { node-name($element) } {
    $element/@* except $element/@ixml:version, $element/node()
  }
};

(: The expected elements of a result's XML assertions. :)
declare function local:expected($result as element()) as element()* {
  for $assertion in $result/(tc:assert-xml | tc:assert-xml-ref)
  return if ($assertion/self::tc:assert-xml) then $assertion/*[1]
         else doc(local:href($assertion))/*
};

declare function local:matches-xml($result as element(), $document as node()) as xs:boolean {
  let $actual := local:without-version($document/*[1])
  return some $expected in local:expected($result) satisfies deep-equal($expected, $actual)
};

declare function local:is-failed($document as node()) as xs:boolean {
  tokenize(string($document/*[1]/@ixml:state)) = "failed"
};

(: Checks a parse (a document, or the error it raised) against result. :)
declare function local:check($result as element(), $outcome as map(*)) as map(*) {
  let $document := $outcome?document
  return
    if (exists($result/tc:assert-dynamic-error) and exists($outcome?error)) then { "outcome": "pass" }
    else if (exists($outcome?error)) then { "outcome": "error", "why": $outcome?error }
    else if (exists($result/tc:assert-not-a-sentence) and local:is-failed($document)) then
      { "outcome": "pass" }
    else if (not(local:is-failed($document)) and local:matches-xml($result, $document)) then
      { "outcome": "pass" }
    else {
      "outcome": "fail",
      "why": "got " || serialize($document/*[1])
    }
};

declare function local:parse($parser as function(*), $input as xs:string) as map(*) {
  try {
    { "document": $parser($input) }
  } catch * {
    { "error": string($err:code) || ": " || $err:description }
  }
};

(: Whether the dependencies of a test set or case are met. :)
declare function local:is-applicable($node as element()) as xs:boolean {
  let $versions := $node/tc:dependencies/@Unicode-version
  return empty($versions) or $versions = $unicode-version
};

declare function local:run-grammar-test($test as element(), $compiled as map(*)) as map(*) {
  let $result := $test/tc:result
  return
    if ($result/tc:assert-not-a-grammar) then
      if (exists($compiled?error)) then { "outcome": "pass" }
      else { "outcome": "fail", "why": "compiled a grammar" }
    else if (exists($compiled?error)) then { "outcome": "error", "why": $compiled?error }
    else
      let $spec := local:grammar-spec($test/parent::*)
      let $form := if ($spec/(self::tc:ixml-grammar | self::tc:ixml-grammar-ref))
                   then local:parse(invisible-xml(), local:grammar($spec))
                   else { "document": document { local:grammar($spec) } }
      return local:check($result, $form)
};

declare function local:run-test-case($case as element(), $compiled as map(*)) as map(*) {
  let $result := $case/tc:result
  return
    if ($result/tc:assert-not-a-grammar) then
      if (exists($compiled?error)) then { "outcome": "pass" }
      else { "outcome": "fail", "why": "compiled a grammar" }
    else if (exists($compiled?error)) then { "outcome": "error", "why": $compiled?error }
    else
      let $input := try { { "input": local:input($case) } }
                    catch * { { "error": string($err:code) || ": " || $err:description } }
      return if (exists($input?error)) then { "outcome": "error", "why": $input?error }
             else local:check($result, local:parse($compiled?parser, $input?input))
};

declare function local:path($node as element()) as xs:string {
  string-join(($node/ancestor-or-self::tc:test-set/@name), "/")
};

(: Runs the test sets of one catalog document, and those it refers to. :)
declare function local:run-catalog($document as document-node()) as map(*)* {
  for $set in $document//tc:test-set
  let $own-grammar := $set/(tc:ixml-grammar | tc:ixml-grammar-ref | tc:vxml-grammar
                            | tc:vxml-grammar-ref)
  let $tests := $set/(tc:grammar-test | tc:test-case)
  where exists($tests)
  let $compiled := if (local:is-applicable($set)) then local:compile(local:grammar-spec($set))
                   else map {}
  for $test in $tests
  let $answer := if (not(local:is-applicable($set) and local:is-applicable($test)))
                 then { "outcome": "not-run", "why": "needs Unicode " ||
                        string-join(($set, $test)/tc:dependencies/@Unicode-version, " or ") }
                 else if ($test/self::tc:grammar-test) then local:run-grammar-test($test, $compiled)
                 else
                   let $case-grammar := $test/(tc:ixml-grammar | tc:ixml-grammar-ref
                                               | tc:vxml-grammar | tc:vxml-grammar-ref)
                   return local:run-test-case(
                     $test,
                     if (exists($case-grammar)) then local:compile(head($case-grammar))
                     else $compiled)
  return map:merge((
    $answer,
    {
      "name": local:path($set) || " " || ($test/@name, "grammar-test")[1],
      "is-grammar-test": exists($test/self::tc:grammar-test),
      "is-correct-grammar": empty($test/tc:result/tc:assert-not-a-grammar)
    }))
};

let $document := doc($catalog)
let $answers := local:run-catalog($document)
let $name := replace($catalog, "^.*third_party/ixml-tests/", "")
let $cases := $answers[not(?is-grammar-test)]
let $grammar-tests := $answers[?is-grammar-test]
let $correct := $cases[?is-correct-grammar and ?outcome != "not-run"]
return string-join((
  for $answer in $answers[?outcome != "pass"]
  return string-join(($answer?outcome, $name, $answer?name, $answer?why), " | "),
  "counts: " || $name ||
    " tests=" || count($answers) ||
    " passed=" || count($answers[?outcome = "pass"]) ||
    " failed=" || count($answers[?outcome = "fail"]) ||
    " errors=" || count($answers[?outcome = "error"]) ||
    " not-run=" || count($answers[?outcome = "not-run"]) ||
    " grammar-tests=" || count($grammar-tests) ||
    " grammar-passed=" || count($grammar-tests[?outcome = "pass"]) ||
    " correct-grammar-cases=" || count($correct) ||
    " correct-grammar-passed=" || count($correct[?outcome = "pass"])
), "&#10;")
