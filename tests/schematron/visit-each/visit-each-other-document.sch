<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="doc('visit-each-other-document/other.xml')//entry">
      <sch:report test="@v &gt; 5">Other entry <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
