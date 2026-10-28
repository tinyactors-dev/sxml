<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="q" uri="urn:example:q"/>
  <sch:pattern>
    <sch:rule context="@q:code">
      <sch:assert test="matches(., '^[A-Z]')">Code <sch:value-of select="."/> starts with a capital.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
