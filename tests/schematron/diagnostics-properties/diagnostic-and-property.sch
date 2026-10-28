<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2" diagnostics="d" properties="p">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d">Change the two.</sch:diagnostic>
  </sch:diagnostics>
  <sch:properties>
    <sch:property id="p">two</sch:property>
  </sch:properties>
</sch:schema>
