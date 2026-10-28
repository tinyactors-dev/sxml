<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:phase id="p" from="/doc/part[xs:integer(@n) gt 0]">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:report test="true()">Item <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
