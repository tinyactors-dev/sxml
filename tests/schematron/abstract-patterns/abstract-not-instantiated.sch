<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="unused">
    <sch:rule context="row">
      <sch:report test="true()">A row.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="table">
      <sch:report test="true()">A table.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
