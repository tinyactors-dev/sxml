<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="a">
    <sch:let name="who" value="'pattern a'"/>
    <sch:rule context="doc">
      <sch:report test="true()">In <sch:value-of select="$who"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:let name="who" value="'pattern b'"/>
    <sch:rule context="doc">
      <sch:report test="true()">In <sch:value-of select="$who"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
