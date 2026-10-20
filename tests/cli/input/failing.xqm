module namespace failing = "http://example.com/failing";
(: A dynamic error raised in a library module: the report names this file. :)
declare function failing:check($value) {
  if ($value lt 0) then error(xs:QName("failing:negative"), "negative") else $value
};
