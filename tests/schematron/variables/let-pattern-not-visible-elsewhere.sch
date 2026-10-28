<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="a">
    <sch:let name="limit" value="2"/>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit + 1">Within the limit.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">Within the limit.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
