<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" queryBinding="xslt3">
  <sch:ns prefix="f" uri="urn:example:functions"/>
  <xsl:function name="f:double" as="xs:integer">
    <xsl:param name="value" as="xs:integer"/>
    <xsl:sequence select="$value * 2"/>
  </xsl:function>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="f:double(xs:integer(@n)) lt 4">Too large.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
