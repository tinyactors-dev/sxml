<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="limit" value="5"/>
  <sch:pattern abstract="true" id="bounded">
    <sch:param name="limit"/>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">Within the limit.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="bounded">
    <sch:param name="limit" value="2"/>
  </sch:pattern>
</sch:schema>
