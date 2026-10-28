<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="a">
    <sch:rule context="item[@n = 3]">
      <sch:report test="true()">Pattern a.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="item">
      <sch:report test="@n = 3">Pattern b.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
