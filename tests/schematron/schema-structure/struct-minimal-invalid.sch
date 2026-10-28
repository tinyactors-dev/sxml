<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2">The n of an item is not 2.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
