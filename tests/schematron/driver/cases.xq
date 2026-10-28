(: The cases of the Schematron suite (tests/schematron/README.md) for the
   driver, sxml-schematron-test (main.c): one string per selected case,
   fields separated by tabs:

     set-name, case-name, schema URI, instance URI, phase, applicability,
     then a name and a value for each schema parameter.

   The phase is empty when the case gives none (5.4.17: #DEFAULT). The
   applicability is "run", or "refuse: BINDING" for a case that needs a
   query binding sxml does not read (xquery, xquery3, xquery31, or a name
   it does not know): such a case does not apply, and the compile must
   fail (6.5). The driver appends a call of local:cases. :)
declare namespace t = "urn:sxml:schematron-test-suite";

declare variable $local:bindings := ("xslt", "xslt2", "xslt3", "xslt4", "xpath", "xpath2",
                                     "xpath3", "xpath31", "xpath4");

declare function local:cases($suite as xs:string, $set-name as xs:string,
                             $case-name as xs:string) as xs:string* {
  for $test-set in doc($suite || "catalog.xml")/t:catalog/t:test-set
  where $set-name = ("", $test-set/@name)
  let $directory := $suite || $test-set/@name || "/"
  for $case in doc($suite || $test-set/@file)/t:test-set/t:test-case
  where $case-name = ("", $case/@name)
  let $refused := $case/t:dependency[@type = "query-binding"][not(@value = $local:bindings)]/@value
  return string-join((
    $test-set/@name, $case/@name,
    $directory || $case/t:schema/@file,
    $directory || $case/t:instance/@file,
    string($case/t:phase),
    if (exists($refused)) then "refuse: " || string-join($refused, " ") else "run",
    for $parameter in $case/t:param return ($parameter/@name, $parameter/@value)
  ), "&#9;")
};
