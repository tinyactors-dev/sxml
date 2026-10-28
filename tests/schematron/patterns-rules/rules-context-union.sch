<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="a | b">
      <sch:report test="true()"><sch:name/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
