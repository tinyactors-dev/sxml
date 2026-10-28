<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:pattern>
    <sch:rule context="e:item">
      <sch:extends href="extends-href-prefix-from-ns/rule.sch"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
