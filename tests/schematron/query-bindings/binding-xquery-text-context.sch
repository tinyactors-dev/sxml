<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xquery31">
  <sch:pattern>
    <sch:rule context="//text()">
      <sch:report test=". = 'x'">Text x.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
