<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:let name="limit" value="2"/>
    <sch:rule context="item">
      <sch:let name="value" value="@n"/>
      <sch:assert test="$value &lt;= $limit">Within the limit.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:let name="value" value="@n * 10"/>
      <sch:report test="$value = 30">Thirty.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
