<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="named">
      <sch:let name="label" value="concat('#', @n)"/>
      <sch:report test="@n = 1">Label <sch:value-of select="$label"/>.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:extends rule="named"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
