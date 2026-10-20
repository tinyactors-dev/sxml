(:~ An HTML page about the loans of the library. :)
import module namespace lib = "https://example.org/library" at "library.xqm";

declare option output:method "html";
declare option output:indent "yes";

declare variable $today as xs:date external := current-date();

declare variable $library := doc("library.xml");
declare variable $members := json-doc("members.json");

(: The loans the report can describe, and the ones it can't. :)
declare variable $loans := doc("loans.xml")//loan;
declare variable $infos := lib:loan-infos($loans, $library, $members);
declare variable $unknown := $loans[not(@book = $library//book/@id)];

<html>
  <head><title>Loans on { $today }</title></head>
  <body>
    <h1>Loans on { $today }</h1>

    <h2>Overdue</h2>
    <ul>{
      for $info in $infos[lib:is-overdue(., $today)]
      order by $info?due
      return <li>{ $info?name } has <cite>{ $info?title }</cite>, due { $info?due }</li>
    }</ul>

    <h2>Members</h2>
    <table>
      <tr><th>Member</th><th>Loans</th><th>Out now</th></tr>
      {
        for $info in $infos
        group by $name := $info?name
        order by $name
        return <tr>
          <td>{ $name }</td>
          <td>{ count($info) }</td>
          <td>{ count($info[empty(?returned)]) }</td>
        </tr>
      }
    </table>

    {
      if (exists($unknown)) {
        <p class="warning">Loans of unknown books: {
          string-join($unknown ! `{ @book } ({ @member }, { @date })`, "; ")
        }</p>
      }
    }
  </body>
</html>
