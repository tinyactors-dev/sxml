<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath31">
  <sch:pattern>
    <sch:rule context="//item">
      <sch:report test="map:get(map { 2: 'two' }, xs:integer(@n)) = 'two'">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
