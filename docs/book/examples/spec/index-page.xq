(: The requirements of the draft as an XHTML page. :)
declare default element namespace "http://www.w3.org/1999/xhtml";
declare option output:indent "yes";

(: Unprefixed names in paths are in the XHTML namespace now, and the
   draft's elements are in none: Q{} names them. :)
declare variable $draft := /Q{}rfc;

<html>
  <head><title>Requirements of { string($draft/@docName) }</title></head>
  <body>
    <h1>{ string($draft/Q{}front/Q{}title) }</h1>
    <ul>{
      for $requirement in $draft//Q{}t[@anchor]
      return
        <li id="{ $requirement/@anchor }">
          <b>{ string($requirement/Q{}bcp14) }</b> in {
            string($requirement/ancestor::Q{}section[1]/Q{}name)
          }
        </li>
    }</ul>
  </body>
</html>
