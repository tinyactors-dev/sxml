<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2" properties="kind value">Two.</sch:report>
      <sch:report test="@n = 3" properties="value">Three.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="kind" role="kind">An <sch:name/>.</sch:property>
    <sch:property id="value" scheme="urn:example:scheme">Value <sch:value-of select="@n"/>.</sch:property>
  </sch:properties>
</sch:schema>
