<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2" properties="p">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="p">Value <sch:value-of select="@n +"/>.</sch:property>
  </sch:properties>
</sch:schema>
