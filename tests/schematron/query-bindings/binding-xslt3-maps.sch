<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="names" value="map { '1': 'one', '2': 'two', '3': 'three' }"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="$names(string(@n)) = 'two'">Named <sch:value-of select="$names(string(@n))"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
