import schema default element namespace "" at "library.xsd";

let $library := validate { / }
for $book in $library//book
where $book/@year lt xs:gYear("1970")
return `{$book/title}: {$book/price * 2}`
