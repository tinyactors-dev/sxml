declare namespace output = "http://www.w3.org/2010/xslt-xquery-serialization";
declare option output:method "json";
declare function local:summary($books as element(book)*) as map(*) {
  map { "count": count($books), "titles": array { $books/title/string() } }
};
local:summary(doc("books.xml")//book)
