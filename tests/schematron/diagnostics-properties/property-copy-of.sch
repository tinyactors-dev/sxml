<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 1" properties="copy">One.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:properties>
    <sch:property id="copy" role="contents">Copied: <xsl:copy-of xmlns:xsl="http://www.w3.org/1999/XSL/Transform" select="string(@n)"/></sch:property>
  </sch:properties>
</sch:schema>
