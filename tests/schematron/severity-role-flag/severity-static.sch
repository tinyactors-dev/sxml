<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 1" severity="warning">Not one.</sch:assert>
      <sch:report test="@n = 3" severity="fatal">Three.</sch:report>
      <sch:report test="@n = 2" severity="minor">A severity outside the reserved values.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
