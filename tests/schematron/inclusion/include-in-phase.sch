<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="second">
    <sch:include href="include-in-phase/active.sch"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:report test="@n = 1">First pattern.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="item">
      <sch:report test="@n = 2">Second pattern.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
