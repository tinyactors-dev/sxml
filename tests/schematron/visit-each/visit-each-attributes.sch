<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="entry/@v">
      <sch:assert test=". &lt; 5">Value <sch:value-of select="."/> below five.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
