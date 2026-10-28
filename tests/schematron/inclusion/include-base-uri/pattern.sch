<?xml version="1.0" encoding="UTF-8"?>
<sch:pattern xmlns:sch="http://purl.oclc.org/dsdl/schematron">
  <sch:rule context="doc">
    <sch:report test="doc('codes.xml')/codes/@from = 'included'">Codes from the included directory.</sch:report>
  </sch:rule>
</sch:pattern>
