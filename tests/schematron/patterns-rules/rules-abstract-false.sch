<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="false">
    <sch:rule context="item" abstract="false">
      <sch:report test="@n = 3">Item three.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
