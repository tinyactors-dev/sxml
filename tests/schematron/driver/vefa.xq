(: The unit tests of a VEFA test file (EN 16931's src/test: a vefa:testSet
   of vefa:test elements, each with a vefa:assert and one embedded
   document) for the driver's vefa command (main.c): for each test, a
   document node holding a copy of its document, then a string with its
   expectations, separated by tabs: each a rule id and "success" (must not
   fire), or "error", "warning", "fatal" with a count ("" for at least
   once), as a pair "ID KIND COUNT". The driver appends a call of
   local:tests. :)
declare namespace vefa = "http://difi.no/xsd/vefa/validator/1.0";

declare function local:tests($file as xs:string) as item()* {
  for $test in doc($file)//vefa:test
  return (
    document { $test/*[not(self::vefa:assert)][1] },
    string-join(
      for $expectation in $test/vefa:assert/(vefa:success | vefa:error | vefa:warning | vefa:fatal)
      return (normalize-space($expectation), local-name($expectation),
              string($expectation/@number)),
      "&#9;"))
};
