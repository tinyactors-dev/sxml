<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern documents="concat(xs:integer(/doc/@n), '.xml')">
    <sch:rule context="item">
      <sch:report test="true()">Item.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
