<?xml version="1.0" encoding="UTF-8"?>
<sch:pattern xmlns:sch="http://purl.oclc.org/dsdl/schematron" xmlns:e="urn:example:other">
  <sch:rule context="e:item">
    <sch:report test="@n = 2">Two.</sch:report>
  </sch:rule>
</sch:pattern>
