<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="q" uri="urn:example:q"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@q:code != 'b2'" subject="@q:code">Not b2.</sch:assert>
      <sch:assert test="@q:code != 'b2'" diagnostics="d" properties="p">Again.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d">Diagnostic <sch:value-of select="@q:code"/>.</sch:diagnostic>
  </sch:diagnostics>
  <sch:properties>
    <sch:property id="p">Eigenschaft ä <sch:value-of select="@q:code"/>.</sch:property>
  </sch:properties>
</sch:schema>
