<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2" properties="currency precision">Priced.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="currency" role="unit" scheme="urn:example:currencies">EUR</sch:property>
    <sch:property id="precision" role="digits">2</sch:property>
  </sch:properties>
</sch:schema>
