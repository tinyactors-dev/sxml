<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="entry">
      <sch:assert test="@v &lt; 5" subject="@v">Entry <sch:value-of select="@v"/> below five.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
