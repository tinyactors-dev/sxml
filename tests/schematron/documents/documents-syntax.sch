<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern documents="('a.xml'">
    <sch:rule context="item">
      <sch:report test="true()">Item.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
