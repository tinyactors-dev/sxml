<?xml version="1.0" encoding="UTF-8"?>
<sch:library xmlns:sch="http://purl.oclc.org/dsdl/schematron">
  <sch:extends href="inner.sch"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 1">Outer.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:library>
