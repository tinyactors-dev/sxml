(:~ Questions about the library and its loans. :)
module namespace lib = "https://example.org/library";

(:~ How many days a loan lasts. :)
declare variable $lib:loan-days as xs:integer := 21;

(:~ The record the report uses for one loan. :)
declare record lib:loan(
  member as xs:string,
  name as xs:string,
  title as xs:string,
  date as xs:date,
  due as xs:date,
  returned as xs:date?
);

(:~ The title of the book with the id, or an error lib:unknown-book. :)
declare function lib:title($library as document-node(), $id as xs:string) as xs:string {
  $library//book[@id = $id]/title/string()
  otherwise error(xs:QName("lib:unknown-book"), "No book has the id " || $id, $id)
};

(:~ One loan, with the names and dates the report needs. :)
declare function lib:loan-info(
  $loan as element(loan),
  $library as document-node(),
  $members as map(*)
) as lib:loan {
  let $date := xs:date($loan/@date)
  return lib:loan(
    member := string($loan/@member),
    name := $members(string($loan/@member))?name otherwise string($loan/@member),
    title := lib:title($library, $loan/@book),
    date := $date,
    due := $date + xs:dayTimeDuration("P" || $lib:loan-days || "D"),
    returned := $loan/@returned ! xs:date(.)
  )
};

(:~ Whether a loan is still out and past its due date on a day. :)
declare function lib:is-overdue($info as lib:loan, $today as xs:date) as xs:boolean {
  empty($info?returned) and $info?due lt $today
};

(:~ The loans of known books, as records; loans of unknown books are left out. :)
declare function lib:loan-infos(
  $loans as element(loan)*,
  $library as document-node(),
  $members as map(*)
) as lib:loan* {
  for $loan in $loans
  return try {
    lib:loan-info($loan, $library, $members)
  } catch lib:unknown-book {
    ()
  }
};
