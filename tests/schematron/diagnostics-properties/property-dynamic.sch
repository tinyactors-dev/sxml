<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="square" value="@n * @n"/>
      <sch:assert test="@n &lt; 3" properties="value">Below three.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="value" role="value"><sch:value-of select="@n"/> squared is <sch:value-of select="$square"/></sch:property>
  </sch:properties>
</sch:schema>
