<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="items" value="count(//item)"/>
  <sch:phase id="first-part" from="/doc/part[1]">
    <sch:active pattern="all"/>
  </sch:phase>
  <sch:pattern id="all">
    <sch:rule context="item">
      <sch:report test="@n = 1">One of <sch:value-of select="$items"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
