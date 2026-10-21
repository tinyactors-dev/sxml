(: A new requirement after req-8, on lines of its own. The whitespace
   between the tags is part of the paragraph, so it is kept. :)
declare boundary-space preserve;

let $after := //t[@anchor = "req-8"]
return insert nodes (
  text { "&#10;      " },
  <t anchor="req-13">A <xref target="term-controller">controller</xref>
      <bcp14>MAY</bcp14> end a lockout early.</t>
) after $after
