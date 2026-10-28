<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="empty" when="/doc/missing">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:phase id="present" when="/doc/item">
    <sch:active pattern="b"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:report test="@n = 1">Pattern a.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="item">
      <sch:report test="@n = 2">Pattern b.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
