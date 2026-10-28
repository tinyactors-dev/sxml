<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@status = 'open'">Open, not <sch:value-of select="@status"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="@status">
      <sch:report test="true()">Status <sch:value-of select="."/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
