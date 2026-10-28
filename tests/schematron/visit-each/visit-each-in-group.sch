<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="list" visit-each="entry">
      <sch:report test="true()">A <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
    <sch:rule context="list" visit-each="entry[@v &gt; 5]">
      <sch:report test="true()">B <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
