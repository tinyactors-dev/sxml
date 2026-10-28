<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" xmlns:p="urn:example:wrong">
  <sch:ns prefix="p" uri="urn:example:right"/>
  <sch:pattern>
    <sch:rule context="p:item">
      <sch:report test="@n = 2">Right namespace.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
