<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:phase id="p" from="/doc/e:part[@n = 2]">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="e:item">
      <sch:report test="true()">Item <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
