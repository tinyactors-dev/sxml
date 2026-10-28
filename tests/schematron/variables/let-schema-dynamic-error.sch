<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:let name="limit" value="xs:integer(/doc/@limit)"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">Within the limit.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
