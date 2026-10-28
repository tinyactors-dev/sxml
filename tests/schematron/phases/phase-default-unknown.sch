<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" defaultPhase="none">
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
  <sch:pattern id="c">
    <sch:rule context="item">
      <sch:report test="@n = 3">Pattern c.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
