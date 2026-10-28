<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="v" value="()"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 1" severity="$v">One.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
