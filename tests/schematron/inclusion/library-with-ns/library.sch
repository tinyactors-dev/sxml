<?xml version="1.0" encoding="UTF-8"?>
<sch:library xmlns:sch="http://purl.oclc.org/dsdl/schematron">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:pattern>
    <sch:rule context="e:item">
      <sch:assert test="false()">Never.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:library>
