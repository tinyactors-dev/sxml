<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example"/>
  <sch:pattern>
    <sch:rule context="e:item">
      <sch:report test="true()">Other schema: <sch:name/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
