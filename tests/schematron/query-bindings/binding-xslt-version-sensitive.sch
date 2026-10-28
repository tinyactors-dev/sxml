<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt">
  <sch:pattern>
    <sch:rule context="doc">
      <sch:report test="'10' &lt; '9'">Compared as strings.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
