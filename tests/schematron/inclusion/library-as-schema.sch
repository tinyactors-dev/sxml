<?xml version="1.0" encoding="UTF-8"?>
<sch:library xmlns:sch="http://purl.oclc.org/dsdl/schematron">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:library>
