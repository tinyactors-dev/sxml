<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="3.0"
    xmlns:f="urn:example:functions" xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:function name="f:is-odd" as="xs:boolean">
    <xsl:param name="value"/>
    <xsl:sequence select="number($value) mod 2 = 1"/>
  </xsl:function>
</xsl:stylesheet>
