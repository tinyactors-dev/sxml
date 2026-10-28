<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath31">
  <sch:pattern>
    <sch:rule context="(//item)[1]">
      <sch:report test="true()">The first item.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
