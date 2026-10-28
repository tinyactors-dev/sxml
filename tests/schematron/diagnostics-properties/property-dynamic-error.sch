<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="true()" properties="p">Reported.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="p">Value <sch:value-of select="xs:integer(@n)"/>.</sch:property>
  </sch:properties>
</sch:schema>
