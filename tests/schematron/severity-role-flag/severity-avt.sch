<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n = 1" severity="{if (@n = 2) then 'warning' else 'error'}">One.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
