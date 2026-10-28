<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 1" properties="unit">Priced.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="unit" role="currency" scheme="urn:example:iso4217">EUR</sch:property>
  </sch:properties>
</sch:schema>
