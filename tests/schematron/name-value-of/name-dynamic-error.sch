<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:report test="true()">Name <sch:name path="item[xs:integer(@n) gt 0]"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
