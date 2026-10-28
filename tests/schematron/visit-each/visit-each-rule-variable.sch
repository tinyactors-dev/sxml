<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="entry">
      <sch:let name="limit" value="@limit"/>
      <sch:assert test="number(@v) lt number($limit)">Entry <sch:value-of select="@v"/> below the limit.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
