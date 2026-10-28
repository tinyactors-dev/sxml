<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2">Plain item.</sch:report>
    </sch:rule>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Special item.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
