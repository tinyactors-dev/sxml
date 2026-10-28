<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:let name="count" value="'three'" as="xs:integer"/>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="$count = 3">Three.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
