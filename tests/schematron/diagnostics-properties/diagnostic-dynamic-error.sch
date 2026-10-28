<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="false()" diagnostics="d">Never holds.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d">Value <sch:value-of select="xs:integer(@n)"/>.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
