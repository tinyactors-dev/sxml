module namespace loans = "https://example.org/loans";

(:~ How many days a loan lasts. :)
declare variable $loans:days as xs:integer := 21;

(:~ The day a loan is due. :)
declare function loans:due($loan as element(loan)) as xs:date {
  loans:start($loan) + loans:period()
};

declare %private function loans:start($loan as element(loan)) as xs:date {
  xs:date($loan/@date)
};

declare %private function loans:period() as xs:dayTimeDuration {
  xs:dayTimeDuration("P" || $loans:days || "D")
};
