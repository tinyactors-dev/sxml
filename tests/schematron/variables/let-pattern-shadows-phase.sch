<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p">
    <sch:let name="limit" value="1"/>
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:let name="limit" value="2"/>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">Within.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
