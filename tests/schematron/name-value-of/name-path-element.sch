<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="order">
      <sch:report test="true()">First child: <sch:name path="*[1]"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
