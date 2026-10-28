<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="doc">
      <sch:report test="true()"><sch:value-of select="count(item)"/> items, <sch:value-of select="sum(item/@n) div 2"/> half, <sch:value-of select="count(item) = 3"/>, <sch:value-of select="upper-case('ok')"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
