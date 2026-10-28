<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="p" uri="urn:example:one"/>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="p:item">Has items of urn:example:one.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
