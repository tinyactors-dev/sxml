<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath31">
  <sch:pattern>
    <sch:rule context="reverse(//item)">
      <sch:assert test="@n != 2">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
