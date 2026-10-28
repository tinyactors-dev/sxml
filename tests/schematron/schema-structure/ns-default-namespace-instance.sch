<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="a" uri="urn:example:n"/>
  <sch:pattern>
    <sch:rule context="a:item">
      <sch:assert test="@n = 1">Only one.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
