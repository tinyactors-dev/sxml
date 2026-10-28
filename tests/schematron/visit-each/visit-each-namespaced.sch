<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:pattern>
    <sch:rule context="e:list" visit-each="e:entry">
      <sch:assert test="@v &lt; 5">Entry <sch:value-of select="@v"/> below five.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
