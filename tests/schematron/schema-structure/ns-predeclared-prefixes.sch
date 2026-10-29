<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="checks" value="(fn:true(), math:pi() gt 3, map:size(map { 'a': 1 }) eq 1, array:size([1, 2]) eq 2, xs:QName('err:FOER0000') eq fn:QName('http://www.w3.org/2005/xqt-errors', 'FOER0000'))"/>
      <sch:assert test="fn:count($checks[.]) eq 5">A prefix of the function library is not bound.</sch:assert>
      <sch:report test="xs:integer(@n) eq 2">Two: <sch:value-of select="fn:string(xs:integer(@n))"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
