<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath2">
  <sch:pattern>
    <sch:rule context="//item">
      <sch:report test="map { 'a': 1 }?a = 1">Maps.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
