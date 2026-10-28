<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="doc" visit-each="tokenize(@codes, ' ')">
      <sch:report test=". = 'b'" properties="p">Found b.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="p">Code <sch:value-of select="."/>.</sch:property>
  </sch:properties>
</sch:schema>
