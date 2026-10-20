module namespace catalog = "https://example.org/catalog";

(:~ The book with the given id, or the empty sequence. :)
declare function catalog:book($library as node(), $id as xs:string) as element(book)? {
  $library//book[@id = $id]
};

(:~ The authors of a book, joined with "and". :)
declare function catalog:authors($book as element(book)) as xs:string {
  string-join($book/author, " and ")
};

(:~ A short line describing a book. :)
declare function catalog:describe($book as element(book)) as xs:string {
  `{$book/title} by {catalog:authors($book)} ({$book/@year})`
};
