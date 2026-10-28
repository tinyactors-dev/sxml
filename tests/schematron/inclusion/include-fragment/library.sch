<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="lib-one">
    <sch:rule context="item">
      <sch:report test="@n = 1">Library pattern one.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="lib-three">
    <sch:rule context="item">
      <sch:report test="@n = 3">Library pattern three.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
