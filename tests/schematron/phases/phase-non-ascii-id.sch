<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="prüfung">
    <sch:active pattern="müster"/>
  </sch:phase>
  <sch:pattern id="müster">
    <sch:rule context="item">
      <sch:report test="@n = 2">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="andere">
    <sch:rule context="item">
      <sch:report test="@n = 1">One.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
