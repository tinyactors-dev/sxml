<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath31">
  <sch:ns prefix="f" uri="urn:example:functions"/>
  <xsl:function xmlns:xsl="http://www.w3.org/1999/XSL/Transform" name="f:double"
      xmlns:f="urn:example:functions">
    <xsl:param name="value"/>
    <xsl:sequence select="number($value) * 2"/>
  </xsl:function>
  <sch:pattern>
    <sch:rule context="//item">
      <sch:report test="f:double(@n) = 4">Doubled to four.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
