<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="p" uri="urn:example:one"/>
  <sch:ns prefix="p" uri="urn:example:two"/>
  <sch:pattern>
    <sch:rule context="p:item">
      <sch:report test="true()">Matched.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
