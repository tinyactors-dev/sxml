<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="a" uri="urn:example:n"/>
  <sch:group>
    <sch:rule context="a:item">
      <sch:report test="@n = 2">Element.</sch:report>
    </sch:rule>
    <sch:rule context="a:item/@n">
      <sch:report test=". = 2">Attribute.</sch:report>
    </sch:rule>
    <sch:rule context="a:*">
      <sch:report test="@n = 2">Any.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
