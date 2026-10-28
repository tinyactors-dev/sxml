<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p" when="(1, 2)">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:report test="@n = 2">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
