<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="d" uri="urn:example:d"/>
  <sch:pattern>
    <sch:rule context="d:item">
      <sch:report test="@n = 2"><sch:name/> two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
